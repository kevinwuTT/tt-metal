#include "tt_dnn/op_library/reduce/reduce_op.hpp"

#include "tt_metal/host_api.hpp"
#include "tt_metal/common/constants.hpp"
#include "tt_metal/detail/util.hpp"

using namespace tt::constants;

namespace tt {

namespace tt_metal {

operation::ProgramWithCallbacks reduce_single_core(const Tensor &a, Tensor& output, ReduceOpMath::Enum reduce_op, ReduceOpDim::Enum reduce_dim, float scaler) {

    const auto shape = a.shape();
    uint32_t W = shape[3], H = shape[2], NC = shape[1]*shape[0];
    uint32_t HW = H*W;

    uint32_t Wt = W/TILE_WIDTH;
    uint32_t Ht = H/TILE_HEIGHT;
    if (reduce_dim == ReduceOpDim::HW)
        scaler = sqrt(scaler);

    uint32_t num_tensor_tiles = NC*H*W / TILE_HW;

    tt_metal::Program program = tt_metal::Program();

    CoreRange core = {.start={0, 0}, .end={0, 0}};


    tt::DataFormat cb_data_format = tt_metal::datatype_to_dataformat_converter(a.dtype());
    uint32_t single_tile_size = tt_metal::detail::TileSize(cb_data_format);

    uint32_t num_tiles = a.volume()/TILE_HW;

    // This should allocate a DRAM buffer on the device
    tt_metal::Device *device = a.device();

    uint32_t src0_cb_index = 0;
    uint32_t num_input_tiles = 2;
    auto cb_src0 = tt_metal::CreateCircularBuffers(
        program,
        src0_cb_index,
        core,
        num_input_tiles,
        num_input_tiles * single_tile_size,
        cb_data_format
    );

    auto cb_src1 = tt_metal::CreateCircularBuffers(
        program,
        CB::c_in2,
        core,
        num_input_tiles,
        num_input_tiles * single_tile_size,
        cb_data_format
    );

    uint32_t output_cb_index = 16; // output operands start at index 16
    uint32_t num_output_tiles = 2;
    auto cb_output = tt_metal::CreateCircularBuffers(
        program,
        output_cb_index,
        core,
        num_output_tiles,
        num_output_tiles * single_tile_size,
        cb_data_format
    );
    // no need to create c_in2 buffer since we pass scaler=0 to reader

    tt_metal::DataMovementKernel *reader_kernel = tt_metal::CreateDataMovementKernel(
        program,
        reduce_dim == ReduceOpDim::H ?
            "tt_metal/kernels/dataflow/reader_unary_transpose_wh_8bank.cpp" :
            "tt_metal/kernels/dataflow/reader_unary_8bank_reduce.cpp",
        core,
        tt_metal::DataMovementProcessor::RISCV_1,
        tt_metal::NOC::RISCV_1_default);

    tt_metal::DataMovementKernel *writer_kernel = tt_metal::CreateDataMovementKernel(
        program,
        "tt_metal/kernels/dataflow/writer_unary_8bank.cpp",
        core,
        tt_metal::DataMovementProcessor::RISCV_0,
        tt_metal::NOC::RISCV_0_default);

    vector<uint32_t> compute_kernel_args = {
        uint32_t(*reinterpret_cast<uint32_t*>(&scaler)), // scaler
        Ht, // Ht
        Wt, // Wt
        NC, // NC
    };
    bool fp32_dest_acc_en = false;
    bool math_approx_mode = false;
    TT_ASSERT(int(reduce_dim) >= 0 && int(reduce_dim) <= ReduceOpDim::all().size());

    string compute_kernel_name = reduce_op_utils::dim_to_kernel_name(reduce_dim, reduce_op);

    auto reduce_compute_kernel = tt_metal::CreateComputeKernel(
        program,
        compute_kernel_name,
        core,
        compute_kernel_args,
        MathFidelity::HiFi4,
        fp32_dest_acc_en,
        math_approx_mode
    );

    reduce_op_utils::add_defines(reduce_compute_kernel, reduce_op, reduce_dim);

    tt_metal::SetRuntimeArgs(
        reader_kernel, core,
        {
            a.buffer()->address(),
            0, // unused by multibank reader
            0, // unused by multibank reader
            num_tensor_tiles, NC, Ht, Wt, Ht*Wt,
            uint32_t(*reinterpret_cast<uint32_t*>(&scaler)),
        }
    );

    uint32_t out_dim_divider = 1;
    switch (reduce_dim) {
        case ReduceOpDim::H: out_dim_divider = Ht; break;
        case ReduceOpDim::W: out_dim_divider = Wt; break;
        case ReduceOpDim::HW: out_dim_divider = Ht*Wt; break;
        default: TT_ASSERT(false && "Unsupported reduce_dim!");
    }

    tt_metal::SetRuntimeArgs(
        writer_kernel, core,
        {
            output.buffer()->address(),
            0, // unused by multibank writer
            0, // unused by multibank writer
            num_tensor_tiles/out_dim_divider
        }
    );

    auto override_runtime_args_callback = [reader_kernel, writer_kernel](
        const std::vector<Buffer*>& input_buffers,
        const std::vector<Buffer*>& output_buffers
    ) {

        auto src_dram_buffer = input_buffers.at(0);

        auto dst_dram_buffer = output_buffers.at(0);

        CoreCoord core = {0, 0};

        {
            auto runtime_args = GetRuntimeArgs(reader_kernel, core);
            runtime_args[0] = src_dram_buffer->address();
            SetRuntimeArgs(reader_kernel, core, runtime_args);
        }

        {
            auto runtime_args = GetRuntimeArgs(writer_kernel, core);
            runtime_args[0] = dst_dram_buffer->address();
            SetRuntimeArgs(writer_kernel, core, runtime_args);
        }
    };

    return {std::move(program), override_runtime_args_callback};
}

}  // namespace tt_metal

}  // namespace tt
