#ifndef BLK_DEVICE_H
#define BLK_DEVICE_H


#include "storage/Devices/Device.h"
#include "storage/Devices/Filesystem.h"


namespace storage
{

    using namespace std;


    // abstract class

    class BlkDevice : public Device
    {
    public:

	virtual string display_name() const override { return getName(); }

	virtual void check() const override;

	const string& getName() const;
	void setName(const string& name);

	static BlkDevice* find(DeviceGraph& device_graph, const string& name);

	Filesystem* createFilesystem(const string& type);

    public:

	class Impl;

	Impl& getImpl();
	const Impl& getImpl() const;

    protected:

	BlkDevice(Impl* impl);
	BlkDevice(DeviceGraph& device_graph, Impl* impl);

    };

}

#endif