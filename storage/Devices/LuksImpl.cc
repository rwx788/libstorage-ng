/*
 * Copyright (c) [2016-2018] SUSE LLC
 *
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, contact Novell, Inc.
 *
 * To contact Novell about this file by physical or electronic mail, you may
 * find current contact information at www.novell.com.
 */


#include "storage/Utils/XmlFile.h"
#include "storage/Utils/SystemCmd.h"
#include "storage/Utils/HumanString.h"
#include "storage/Utils/CallbacksImpl.h"
#include "storage/Devices/LuksImpl.h"
#include "storage/Holders/User.h"
#include "storage/Devicegraph.h"
#include "storage/Action.h"
#include "storage/StorageImpl.h"
#include "storage/SystemInfo/SystemInfo.h"
#include "storage/UsedFeatures.h"
#include "storage/EtcCrypttab.h"
#include "storage/Prober.h"


namespace storage
{

    using namespace std;


    const char* DeviceTraits<Luks>::classname = "Luks";


    Luks::Impl::Impl(const xmlNode* node)
	: Encryption::Impl(node), uuid()
    {
	getChildValue(node, "uuid", uuid);
    }


    string
    Luks::Impl::get_pretty_classname() const
    {
	// TRANSLATORS: name of object
	return _("LUKS").translated;
    }


    void
    Luks::Impl::save(xmlNode* node) const
    {
	Encryption::Impl::save(node);

	setChildValue(node, "uuid", uuid);
    }


    void
    Luks::Impl::check(const CheckCallbacks* check_callbacks) const
    {
	BlkDevice::Impl::check(check_callbacks);

	if (!has_single_parent_of_type<const BlkDevice>())
	    ST_THROW(Exception("Luks has no BlkDevice parent"));

	if (get_size() + metadata_size > get_blk_device()->get_size())
	    ST_THROW(Exception("Luks bigger than parent BlkDevice"));
    }


    string
    Luks::Impl::get_mount_by_name(MountByType mount_by_type) const
    {
	string ret;

	switch (mount_by_type)
	{
	    case MountByType::UUID:
		if (!uuid.empty())
		    ret = "UUID=" + uuid;
		else
		    y2err("no uuid defined, using fallback");
		break;

	    case MountByType::LABEL:
	    case MountByType::ID:
	    case MountByType::PATH:
	    case MountByType::DEVICE:
		break;
	}

	if (ret.empty())
	{
	    ret = Encryption::Impl::get_mount_by_name(mount_by_type);
	}

	return ret;
    }


    string
    Luks::Impl::next_free_cr_auto_name(SystemInfo& system_info)
    {
	const CmdDmsetupInfo& cmd_dmsetup_info = system_info.getCmdDmsetupInfo();
	const EtcCrypttab& etc_crypttab = system_info.getEtcCrypttab();

	static int nr = 1;

	while (true)
	{
	    string name = "cr-auto-" + to_string(nr++);

	    if (!cmd_dmsetup_info.exists(name) && !etc_crypttab.has_crypt_device(name))
		return name;
	}
    }


    bool
    Luks::Impl::activate_luks(const ActivateCallbacks* activate_callbacks, SystemInfo& system_info,
			      const string& name, const string& uuid)
    {
	int attempt = 1;
	string dm_name;

	while (true)
	{
	    pair<bool, string> tmp = activate_callbacks->luks(uuid, attempt);
	    if (!tmp.first)
	    {
		y2mil("user canceled activation of luks " << uuid);
		return false;
	    }
	    else
	    {
		y2mil("user allowed activation of luks " << uuid);
	    }

	    // TRANSLATORS: progress message
	    message_callback(activate_callbacks, sformat(_("Activating LUKS %s"), uuid.c_str()));

	    if (attempt == 1)
	    {
		dev_t majorminor = system_info.getCmdUdevadmInfo(name).get_majorminor();

		const EtcCrypttab& etc_crypttab = system_info.getEtcCrypttab();
		const CrypttabEntry* crypttab_entry = etc_crypttab.find_by_block_device(system_info, uuid,
											"", majorminor);

		if (crypttab_entry)
		    dm_name = crypttab_entry->get_crypt_device();
		else
		    dm_name = next_free_cr_auto_name(system_info);
	    }

	    string cmd_line = CRYPTSETUPBIN " --batch-mode luksOpen " + quote(name) + " " +
		quote(dm_name) + " --tries 1 --key-file -";

	    SystemCmd::Options cmd_options(cmd_line, SystemCmd::DoThrow);
	    cmd_options.stdin_text = tmp.second;
	    cmd_options.verify = [](int exit_code) { return exit_code == 0 || exit_code == 2; };

	    try
	    {
		SystemCmd cmd(cmd_options);

		// check for wrong password
		if (cmd.retcode() == 2)
		{
		    attempt++;
		    continue;
		}

		return true;
	    }
	    catch (const Exception& exception)
	    {
		// TRANSLATORS: error message
		error_callback(activate_callbacks, sformat(_("Activating LUKS %s failed"), uuid.c_str()),
			       exception);

		return false;
	    }
	}
    }


    bool
    Luks::Impl::activate_lukses(const ActivateCallbacks* activate_callbacks)
    {
	y2mil("activate_lukses");

	try
	{
	    SystemInfo system_info;

	    bool ret = false;

	    for (const Blkid::value_type& key_value1 : system_info.getBlkid())
	    {
		if (!key_value1.second.is_luks)
		    continue;

		// major and minor of the device holding the luks
		dev_t majorminor = system_info.getCmdUdevadmInfo(key_value1.first).get_majorminor();

		const CmdDmsetupTable& dmsetup_table = system_info.getCmdDmsetupTable();
		CmdDmsetupTable::const_iterator it = dmsetup_table.find_using(majorminor);
		if (it != dmsetup_table.end())
		    continue;

		y2mil("inactive luks name:" << key_value1.first << " uuid:" <<
		      key_value1.second.luks_uuid);

		// TODO During a second loop of Storage::Impl::activate() the
		// library should not bother the user with popups to lukses where
		// an activation was canceled by the user. Maybe the library
		// should also remember the passwords (map<uuid, password>) in
		// case the activation is run again, e.g. after deactivation and
		// reprobe.

		if (activate_luks(activate_callbacks, system_info, key_value1.first,
				  key_value1.second.luks_uuid))
		    ret = true;
	    }

	    if (ret)
		SystemCmd(UDEVADMBIN_SETTLE);

	    return ret;
	}
	catch (const Exception& exception)
	{
	    ST_CAUGHT(exception);

	    if (typeid(exception) == typeid(Aborted))
		ST_RETHROW(exception);

	    // Ignore failure to detect whether LUKSes needs to be activated.

	    return false;
	}
    }


    bool
    Luks::Impl::deactivate_lukses()
    {
	y2mil("deactivate_lukses");

	bool ret = true;

	SystemInfo system_info;
	for (const CmdDmsetupInfo::value_type& value : system_info.getCmdDmsetupInfo())
	{
	    if (value.second.subsystem != "CRYPT")
		continue;

	    string cmd_line = CRYPTSETUPBIN " --batch-mode close " + quote(value.first);

	    SystemCmd cmd(cmd_line);

	    if (cmd.retcode() != 0)
		ret = false;
	}

	return ret;
    }


    void
    Luks::Impl::probe_lukses(Prober& prober)
    {
	SystemInfo& system_info = prober.get_system_info();
	const CmdDmsetupTable& cmd_dmsetup_table = system_info.getCmdDmsetupTable();
	const EtcCrypttab& etc_crypttab = system_info.getEtcCrypttab();
	const Blkid& blkid = system_info.getBlkid();

	/*
	 * The main source for probing LUKSes is the blkid output. It contains
	 * all LUKSes with UUID and underlying block device. For some LUKSes
	 * this is already all available information.
	 *
	 * Only search for LUKSes on block devices without children. Otherwise
	 * e.g. LUKSes directly on multipath devices would be detected several
	 * times.
	 */

	for (BlkDevice* blk_device : BlkDevice::get_all(prober.get_system()))
	{
	    if (blk_device->has_children())
		continue;

	    if (!blk_device->get_impl().is_active())
		continue;

	    Blkid::const_iterator it1 = blkid.find_by_name(blk_device->get_name(), system_info);
	    if (it1 == blkid.end() || !it1->second.is_luks)
		continue;

	    string uuid = it1->second.luks_uuid;

	    dev_t majorminor = system_info.getCmdUdevadmInfo(blk_device->get_name()).get_majorminor();

	    CmdDmsetupTable::const_iterator it2 = cmd_dmsetup_table.find_using(majorminor);

	    const CrypttabEntry* crypttab_entry = etc_crypttab.find_by_block_device(system_info, uuid,
										    "", majorminor);

	    /*
	     * The DM table name is 1. taken from the active table, 2. taken
	     * from crypttab and 3. auto-generated.
	     */
	    string dm_table_name;
	    if (it2 != cmd_dmsetup_table.end())
		dm_table_name = it2->first;
	    else if (crypttab_entry)
		dm_table_name = crypttab_entry->get_crypt_device();
	    else
		dm_table_name = next_free_cr_auto_name(system_info);

	    Luks* luks = Luks::create(prober.get_system(), dm_table_name);
	    luks->get_impl().uuid = uuid;
	    luks->get_impl().set_active(it2 != cmd_dmsetup_table.end());
	    luks->set_in_etc_crypttab(crypttab_entry);

	    if (crypttab_entry)
	    {
		luks->set_mount_by(crypttab_entry->get_mount_by());
		luks->get_impl().set_crypt_options(crypttab_entry->get_crypt_opts());
		luks->get_impl().set_crypttab_blk_device_name(crypttab_entry->get_block_device());
	    }

	    prober.add_holder(blk_device->get_name(), luks, [](Devicegraph* system, Device* a, Device* b) {
		User::create(system, a, b);
	    });

	    luks->get_impl().probe_pass_1a(prober);
	}
    }


    void
    Luks::Impl::probe_pass_1a(Prober& prober)
    {
	Encryption::Impl::probe_pass_1a(prober);

	if (is_active())
	{
	    const File size_file = prober.get_system_info().getFile(SYSFS_DIR + get_sysfs_path() + "/size");
	    set_region(Region(0, size_file.get<unsigned long long>(), 512));
	}
    }


    void
    Luks::Impl::probe_uuid()
    {
	const BlkDevice* blk_device = get_blk_device();

	const Blkid& blkid(blk_device->get_name());
	Blkid::const_iterator it = blkid.get_sole_entry();
	if (it != blkid.end())
	    uuid = it->second.luks_uuid;
    }


    void
    Luks::Impl::parent_has_new_region(const Device* parent)
    {
	calculate_region();
    }


    void
    Luks::Impl::calculate_region()
    {
	const BlkDevice* blk_device = get_blk_device();

	unsigned long long size = blk_device->get_size();

	// size of luks metadata is explained at
	// https://gitlab.com/cryptsetup/cryptsetup/wikis/FrequentlyAskedQuestions

	if (size > metadata_size)
	    size -= metadata_size;
	else
	    size = 0 * B;

	set_size(size);
    }


    bool
    Luks::Impl::equal(const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	if (!Encryption::Impl::equal(rhs))
	    return false;

	return uuid == rhs.uuid;
    }


    void
    Luks::Impl::log_diff(std::ostream& log, const Device::Impl& rhs_base) const
    {
	const Impl& rhs = dynamic_cast<const Impl&>(rhs_base);

	Encryption::Impl::log_diff(log, rhs);

	storage::log_diff(log, "uuid", uuid, rhs.uuid);
    }


    void
    Luks::Impl::print(std::ostream& out) const
    {
	Encryption::Impl::print(out);

	out << " uuid:" << uuid;
    }


    ResizeInfo
    Luks::Impl::detect_resize_info() const
    {
	ResizeInfo resize_info = BlkDevice::Impl::detect_resize_info();

	resize_info.shift(metadata_size);

	return resize_info;
    }


    uint64_t
    Luks::Impl::used_features() const
    {
	return UF_LUKS | Encryption::Impl::used_features();
    }


    void
    Luks::Impl::do_create()
    {
	const BlkDevice* blk_device = get_blk_device();

	string cmd_line = CRYPTSETUPBIN " --batch-mode luksFormat " + quote(blk_device->get_name()) +
	    " --tries 1 --key-file -";

	SystemCmd::Options cmd_options(cmd_line, SystemCmd::DoThrow);
	cmd_options.stdin_text = get_password();

	wait_for_devices({ blk_device });

	SystemCmd cmd(cmd_options);

	probe_uuid();
    }


    void
    Luks::Impl::do_delete() const
    {
	const BlkDevice* blk_device = get_blk_device();

	string cmd_line = CRYPTSETUPBIN " --batch-mode erase " + quote(blk_device->get_name());

	SystemCmd cmd(cmd_line, SystemCmd::DoThrow);

	// cryptsetup erase does not remove the signature, thus also use
	// generic wipefs.

	blk_device->get_impl().wipe_device();
    }


    void
    Luks::Impl::do_resize(ResizeMode resize_mode, const Device* rhs) const
    {
	const Luks* luks_rhs = to_luks(rhs);

	string cmd_line = CRYPTSETUPBIN " resize " + quote(get_dm_table_name());

	if (resize_mode == ResizeMode::SHRINK)
	    cmd_line += " --size " + to_string(luks_rhs->get_impl().get_size() / (512 * B));

	SystemCmd cmd(cmd_line, SystemCmd::DoThrow);
    }


    void
    Luks::Impl::do_activate() const
    {
	const BlkDevice* blk_device = get_blk_device();

	string cmd_line = CRYPTSETUPBIN " --batch-mode luksOpen " + quote(blk_device->get_name()) + " " +
	    quote(get_dm_table_name()) + " --tries 1 --key-file -";

	SystemCmd::Options cmd_options(cmd_line, SystemCmd::DoThrow);
	cmd_options.stdin_text = get_password();

	wait_for_devices({ blk_device });

	SystemCmd cmd(cmd_options);
    }


    void
    Luks::Impl::do_deactivate() const
    {
	string cmd_line = CRYPTSETUPBIN " --batch-mode close " + quote(get_dm_table_name());

	SystemCmd cmd(cmd_line, SystemCmd::DoThrow);
    }


    void
    Luks::Impl::do_add_to_etc_crypttab(CommitData& commit_data) const
    {
	EtcCrypttab& etc_crypttab = commit_data.get_etc_crypttab();

	CrypttabEntry* entry = new CrypttabEntry();
	entry->set_crypt_device(get_dm_table_name());
	entry->set_block_device(get_mount_by_name(get_mount_by()));
	entry->set_crypt_opts(get_crypt_options());

	etc_crypttab.add(entry);
	etc_crypttab.log();
	etc_crypttab.write();
    }


    void
    Luks::Impl::do_rename_in_etc_crypttab(CommitData& commit_data) const
    {
	EtcCrypttab& etc_crypttab = commit_data.get_etc_crypttab();

	CrypttabEntry* entry = etc_crypttab.find_block_device(get_crypttab_blk_device_name());
	if (entry)
	{
	    entry->set_block_device(get_mount_by_name(get_mount_by()));
	    etc_crypttab.log();
	    etc_crypttab.write();
	}
    }


    void
    Luks::Impl::do_remove_from_etc_crypttab(CommitData& commit_data) const
    {
	EtcCrypttab& etc_crypttab = commit_data.get_etc_crypttab();

	CrypttabEntry* entry = etc_crypttab.find_block_device(get_crypttab_blk_device_name());
	if (entry)
	{
	    etc_crypttab.remove(entry);
	    etc_crypttab.log();
	    etc_crypttab.write();
	}
    }

}
