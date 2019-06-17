#include "pch.h"
#ifdef _WIN32
#include "rthsLog.h"
#include "rthsMisc.h"
#include "rthsGfxContextDXR.h"
#include "rthsDeformerDXR.h"

// shader binaries
#include "rthsDeform.h"

namespace rths {

enum DeformFlag
{
    DF_APPLY_BLENDSHAPE = 1,
    DF_APPLY_SKINNING = 2,
};

struct BoneCount
{
    int weight_count;
    int weight_offset;
};

struct MeshInfo
{
    int vertex_count;
    int vertex_stride; // in element (e.g. 6 if position + normals)
    int deform_flags;
    int blendshape_count;
};


static const D3D12_DESCRIPTOR_RANGE g_descriptor_ranges[] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 1 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 2 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2, 0, 3 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3, 0, 4 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4, 0, 5 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5, 0, 6 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 7 },
};


DeformerDXR::DeformerDXR(ID3D12Device5Ptr device)
    : m_device(device)
{
    {
        {
            D3D12_COMMAND_QUEUE_DESC desc{};
            desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
            desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_cmd_queue));
        }
        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_cmd_allocator));
        m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_cmd_allocator, nullptr, IID_PPV_ARGS(&m_cmd_list));
        m_cmd_list->Close();
    }

    {
        D3D12_ROOT_PARAMETER params[_countof(g_descriptor_ranges)]{};
        for (int i = 0; i < _countof(g_descriptor_ranges); i++) {
            auto& param = params[i];
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.DescriptorTable.NumDescriptorRanges = 1;
            param.DescriptorTable.pDescriptorRanges = &g_descriptor_ranges[i];
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc{};
        desc.NumParameters = _countof(params);
        desc.pParameters = params;

        ID3DBlobPtr sig_blob;
        ID3DBlobPtr error_blob;
        HRESULT hr = ::D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &error_blob);
        if (FAILED(hr)) {
            SetErrorLog(ToString(error_blob) + "\n");
        }
        else {
            hr = m_device->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&m_rootsig_deform));
            if (FAILED(hr)) {
                SetErrorLog("CreateRootSignature() failed\n");
            }
        }
    }

    if (m_rootsig_deform) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psd {};
        psd.pRootSignature = m_rootsig_deform.GetInterfacePtr();
        psd.CS.pShaderBytecode = rthsDeform;
        psd.CS.BytecodeLength = sizeof(rthsDeform);

        HRESULT hr = m_device->CreateComputePipelineState(&psd, IID_PPV_ARGS(&m_pipeline_state));
        if (FAILED(hr)) {
            SetErrorLog("CreateComputePipelineState() failed\n");
        }
    }
}

DeformerDXR::~DeformerDXR()
{
}

bool DeformerDXR::prepare()
{
    bool ret = false;
    if (SUCCEEDED(m_cmd_allocator->Reset())) {
        if (SUCCEEDED(m_cmd_list->Reset(m_cmd_allocator, m_pipeline_state))) {
            ret = true;
        }
    }
    return ret;
}

bool DeformerDXR::queueDeformCommand(MeshInstanceDataDXR& inst_dxr)
{
    if (!m_rootsig_deform || !m_pipeline_state || !inst_dxr.mesh)
        return false;

    auto& inst = *inst_dxr.base;
    auto& mesh = *inst_dxr.mesh->base;
    auto& mesh_dxr = *inst_dxr.mesh;

    int vertex_count = mesh.vertex_count;
    int blendshape_count = (int)inst.blendshape_weights.size();
    int bone_count = (int)inst.bones.size();

    if (blendshape_count == 0 && bone_count == 0)
        return false; // no need to deform

    // setup descriptors
    if (!inst_dxr.srvuav_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.NumDescriptors = _countof(g_descriptor_ranges);
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&inst_dxr.srvuav_heap));
    }
    auto handle_allocator = DescriptorHeapAllocatorDXR(m_device, inst_dxr.srvuav_heap);
    auto hdst_vertices = handle_allocator.allocate();
    auto hbase_vertices = handle_allocator.allocate();
    auto hbs_point_delta = handle_allocator.allocate();
    auto hbs_point_weights = handle_allocator.allocate();
    auto hbone_counts = handle_allocator.allocate();
    auto hbone_weights = handle_allocator.allocate();
    auto hbone_matrices = handle_allocator.allocate();
    auto hmesh_info = handle_allocator.allocate();

    if (!inst_dxr.deformed_vertices) {
        // deformed vertices
        inst_dxr.deformed_vertices = createBuffer(sizeof(float4) * vertex_count, kDefaultHeapProps, true);
        createUAV(hdst_vertices.hcpu, inst_dxr.deformed_vertices, vertex_count, sizeof(float4));

        // base vertices
        // todo: handle mesh.vertex_offset
        createSRV(hbase_vertices.hcpu, mesh_dxr.vertex_buffer->resource, vertex_count, mesh_dxr.getVertexStride());
    }

    // blendshape
    if (blendshape_count > 0) {
        if (!mesh_dxr.bs_point_delta) {
            // point delta
            mesh_dxr.bs_point_delta = createBuffer(sizeof(float4) * vertex_count * blendshape_count, kUploadHeapProps);
            createSRV(hbs_point_delta.hcpu, mesh_dxr.bs_point_delta, vertex_count * blendshape_count, sizeof(float4));
            writeBuffer(mesh_dxr.bs_point_delta, [&](void *dst_) {
                auto dst = (float4*)dst_;
                for (int bsi = 0; bsi < blendshape_count; ++bsi) {
                    auto& delta = mesh.blendshapes[bsi].frames[0].delta;
                    for (int vi = 0; vi < vertex_count; ++vi)
                        *dst++ = to_float4(delta[vi], 0.0f);
                }
            });
        }

        // weights
        {
            if (!inst_dxr.blendshape_weights) {
                inst_dxr.blendshape_weights = createBuffer(sizeof(float) * blendshape_count, kUploadHeapProps);
                createSRV(hbs_point_weights.hcpu, inst_dxr.blendshape_weights, blendshape_count, sizeof(float));
            }
            // update on every frame
            writeBuffer(inst_dxr.blendshape_weights, [&](void *dst_) {
                std::copy(inst.blendshape_weights.data(), inst.blendshape_weights.data() + inst.blendshape_weights.size(),
                    (float*)dst_);
                });
        }
    }

    // skinning 
    if (bone_count > 0) {
        // bone counts & weights
        if (!mesh_dxr.bone_counts) {
            mesh_dxr.bone_counts = createBuffer(sizeof(BoneCount) * vertex_count, kUploadHeapProps);
            createSRV(hbone_counts.hcpu, mesh_dxr.bone_counts, vertex_count, sizeof(BoneCount));

            int weight_count = 0;
            writeBuffer(mesh_dxr.bone_counts, [&](void *dst_) {
                auto dst = (BoneCount*)dst_;
                for (int vi = 0; vi < vertex_count; ++vi) {
                    int n = mesh.skin.bone_counts[vi];
                    *dst++ = { n, weight_count };
                    weight_count += n;
                }
            });

            mesh_dxr.bone_weights = createBuffer(sizeof(BoneWeight) * weight_count, kUploadHeapProps);
            createSRV(hbone_weights.hcpu, mesh_dxr.bone_weights, weight_count, sizeof(BoneWeight));
            writeBuffer(mesh_dxr.bone_weights, [&](void *dst_) {
                auto dst = (BoneWeight*)dst_;
                for (int wi = 0; wi < weight_count; ++wi) {
                    auto& w1 = mesh.skin.weights[wi];
                    *dst++ = { w1.weight, w1.index };
                }
            });
        }

        // bone matrices
        {
            if (!inst_dxr.bones) {
                inst_dxr.bones = createBuffer(sizeof(float4x4) * bone_count, kUploadHeapProps);
                createSRV(hbone_matrices.hcpu, inst_dxr.bones, bone_count, sizeof(float4x4));
            }
            // update on every frame
            writeBuffer(inst_dxr.bones, [&](void *dst_) {
                auto dst = (float4x4*)dst_;

                auto iroot = invert(inst.transform);
                for (int bi = 0; bi < bone_count; ++bi) {
                    *dst++ = mesh.skin.bindposes[bi] * inst.bones[bi] * iroot;
                }
            });
        }
    }

    // mesh info
    if (!mesh_dxr.mesh_info) {
        int size = align_to(256, sizeof(MeshInfo));
        mesh_dxr.mesh_info = createBuffer(size, kUploadHeapProps);
        createCBV(hmesh_info.hcpu, mesh_dxr.mesh_info, size);
        writeBuffer(mesh_dxr.mesh_info, [&](void *dst_) {
            MeshInfo info{};
            info.vertex_count = vertex_count;
            info.vertex_stride = mesh_dxr.getVertexStride() / 4;
            info.deform_flags = 0;
            if (blendshape_count > 0)
                info.deform_flags |= DF_APPLY_BLENDSHAPE;
            if (bone_count > 0)
                info.deform_flags |= DF_APPLY_SKINNING;
            info.blendshape_count = blendshape_count;

            *(MeshInfo*)dst_ = info;
        });
    }

    {
        m_cmd_list->SetComputeRootSignature(m_rootsig_deform);

        ID3D12DescriptorHeap* heaps[] = { inst_dxr.srvuav_heap };
        m_cmd_list->SetDescriptorHeaps(_countof(heaps), heaps);
        m_cmd_list->SetComputeRootDescriptorTable(0, hdst_vertices.hgpu);
        m_cmd_list->SetComputeRootDescriptorTable(1, hbase_vertices.hgpu);
        m_cmd_list->SetComputeRootDescriptorTable(2, hbs_point_delta.hgpu);
        m_cmd_list->SetComputeRootDescriptorTable(3, hbs_point_weights.hgpu);
        m_cmd_list->SetComputeRootDescriptorTable(4, hbone_counts.hgpu);
        m_cmd_list->SetComputeRootDescriptorTable(5, hbone_weights.hgpu);
        m_cmd_list->SetComputeRootDescriptorTable(6, hbone_matrices.hgpu);
        m_cmd_list->SetComputeRootDescriptorTable(7, hmesh_info.hgpu);

        m_cmd_list->Dispatch(mesh.vertex_count, 1, 1);
    }

    return true;
}

bool DeformerDXR::executeDeform(ID3D12FencePtr fence, UINT64 fence_value)
{
    if(FAILED(m_cmd_list->Close()))
        return false;

    ID3D12CommandList* cmd_list[] = { m_cmd_list.GetInterfacePtr() };
    m_cmd_queue->ExecuteCommandLists(_countof(cmd_list), cmd_list);
    m_cmd_queue->Signal(fence, fence_value);
    return true;
}


void DeformerDXR::createSRV(D3D12_CPU_DESCRIPTOR_HANDLE dst, ID3D12Resource *res, int num_elements, int stride)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = num_elements;
    desc.Buffer.StructureByteStride = stride;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    m_device->CreateShaderResourceView(res, &desc, dst);
}

void DeformerDXR::createUAV(D3D12_CPU_DESCRIPTOR_HANDLE dst, ID3D12Resource *res, int num_elements, int stride)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = num_elements;
    desc.Buffer.StructureByteStride = stride;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    m_device->CreateUnorderedAccessView(res, nullptr, &desc, dst);
}

void DeformerDXR::createCBV(D3D12_CPU_DESCRIPTOR_HANDLE dst, ID3D12Resource *res, int size)
{
    D3D12_CONSTANT_BUFFER_VIEW_DESC desc{};
    desc.BufferLocation = res->GetGPUVirtualAddress();
    desc.SizeInBytes = size;
    m_device->CreateConstantBufferView(&desc, dst);
}


ID3D12ResourcePtr DeformerDXR::createBuffer(int size, const D3D12_HEAP_PROPERTIES& heap_props, bool uav)
{
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;

    ID3D12ResourcePtr ret;
    auto hr = m_device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ret));
    if (FAILED(hr)) {
        SetErrorLog("CreateCommittedResource() failed\n");
    }
    return ret;
}

template<class Body>
bool DeformerDXR::writeBuffer(ID3D12Resource *res, const Body& body)
{
    void *data;
    auto hr = res->Map(0, nullptr, &data);
    if (SUCCEEDED(hr)) {
        body(data);
        res->Unmap(0, nullptr);
        return true;
    }
    else {
        SetErrorLog("Map() failed\n");
    }
    return false;
}


} // namespace rths
#endif // _WIN32