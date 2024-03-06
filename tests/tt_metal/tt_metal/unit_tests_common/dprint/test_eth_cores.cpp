// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "dprint_fixture.hpp"
#include "gtest/gtest.h"
#include "test_utils.hpp"
#include "tt_metal/detail/tt_metal.hpp"
#include "tt_metal/host_api.hpp"

////////////////////////////////////////////////////////////////////////////////
// A test for printing from ethernet cores.
////////////////////////////////////////////////////////////////////////////////
using namespace tt;
using namespace tt::tt_metal;

const std::string golden_output =
R"(Test Debug Print: ERISC
Basic Types:
101-1.618@0.122559
e5551234569123456789
-17-343-44444-5123456789
SETPRECISION/FIXED/DEFAULTFLOAT:
3.1416
3.14159012
3.14159
3.141590118
SETW:
    123456123456  ab
HEX/OCT/DEC:
1e240361100123456)";

static void RunTest(DPrintFixture* fixture, Device* device) {
    // Try printing on all ethernet cores on this device
    int count = 0;
    for (const auto& core : device->get_active_ethernet_cores(true)) {
        // Set up program and command queue
        Program program = Program();

        // Create the kernel
        // TODO: When #5566 is implemented combine these kernels again.
        KernelHandle erisc_kernel_id = CreateKernel(
            program,
            "tests/tt_metal/tt_metal/test_kernels/misc/erisc_print.cpp",
            core,
            tt_metal::EthernetConfig{.noc = tt_metal::NOC::NOC_0});

        // Run the program
        log_info(
            tt::LogTest,
            "Running print test on eth core {}:({},{})",
            device->id(),
            core.x,
            core.y
        );
        fixture->RunProgram(device, program);

        program = Program();
        erisc_kernel_id = CreateKernel(
            program,
            "tests/tt_metal/tt_metal/test_kernels/misc/erisc_print_modifiers.cpp",
            core,
            tt_metal::EthernetConfig{
                .noc = tt_metal::NOC::NOC_0
            }
        );
        fixture->RunProgram(device, program);

        // Check the print log against golden output.
        EXPECT_TRUE(
            FilesMatchesString(
                DPrintFixture::dprint_file_name,
                golden_output
            )
        );

        // Clear the log file for the next core's test
        tt::DPrintServerClearLogFile();
    }
}

TEST_F(DPrintFixture, TestPrintEthCores) {
    for (Device* device : this->devices_) {
        // Skip if no ethernet cores on this device
        if (device->get_active_ethernet_cores(true).size() == 0) {
            log_info(tt::LogTest, "Skipping device {} due to no ethernet cores...", device->id());
            continue;
        }
        this->RunTestOnDevice(RunTest, device);
    }
}
