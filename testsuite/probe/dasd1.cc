
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE libstorage

#include <iostream>
#include <boost/test/unit_test.hpp>

#include "storage/Environment.h"
#include "storage/Storage.h"
#include "storage/DevicegraphImpl.h"
#include "storage/UsedFeatures.h"

#include "testsuite/helpers/TsCmp.h"


using namespace std;
using namespace storage;


BOOST_AUTO_TEST_CASE(dependencies)
{
    set_logger(get_stdout_logger());

    Environment environment(true, ProbeMode::READ_MOCKUP, TargetMode::DIRECT);
    environment.set_mockup_filename("dasd1-mockup.xml");

    Storage storage(environment);
    storage.probe();

    const Devicegraph* probed = storage.get_probed();
    // probed->save("dasd1-devicegraph.xml");

    probed->check();

    Devicegraph* staging = storage.get_staging();

    staging->load("dasd1-devicegraph.xml");
    staging->check();

    TsCmpDevicegraph cmp(*probed, *staging);
    BOOST_CHECK_MESSAGE(cmp.ok(), cmp);
}