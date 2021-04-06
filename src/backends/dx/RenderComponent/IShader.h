#pragma once
#include <Common/GFXUtil.h>
#include <Common/VObject.h>
#include <CJsonObject/SerializedObject.h>
#include <Struct/ShaderVariableType.h>
class DescriptorHeap;
class StructuredBuffer;
class UploadBuffer;
class Mesh;
class TextureBase;
class RenderTexture;
class ShaderLoader;
class ThreadCommand;
class ShaderIO;
class VENGINE_DLL_RENDERER IShader : public VObject {
	friend class ShaderIO;

protected:
	Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
	HashMap<uint, uint> mVariablesDict;
	vengine::vector<ShaderVariable> mVariablesVector;
	bool VariableReflection(uint id, void const* ptr, uint& rootSigPos, ShaderVariable& varResult) const;

public:
	~IShader() {}
	virtual SerializedObject const* GetJsonObject() const = 0;
	size_t VariableLength() const { return mVariablesVector.size(); }
	int32_t GetPropertyRootSigPos(uint id) const;
	virtual void BindShader(ThreadCommand* commandList) const = 0;
	virtual void BindShader(ThreadCommand* commandList, const DescriptorHeap* heap) const = 0;
	virtual bool SetBufferByAddress(ThreadCommand* commandList, uint id, GpuAddress address) const = 0;
	virtual vengine::string const& GetName() const = 0;
	virtual bool SetResource(ThreadCommand* commandList, uint id, DescriptorHeap const* descHeap, uint64 elementOffset) const = 0;
	virtual bool SetResource(ThreadCommand* commandList, uint id, UploadBuffer const* buffer, uint64 elementOffset) const = 0;
	virtual bool SetResource(ThreadCommand* commandList, uint id, StructuredBuffer const* buffer, uint64 elementOffset) const = 0;
	virtual bool SetResource(ThreadCommand* commandList, uint id, Mesh const* mesh, uint64 byteOffset) const = 0;
	virtual bool SetResource(ThreadCommand* commandList, uint id, TextureBase const* texture) const = 0;
	virtual bool SetResource(ThreadCommand* commandList, uint id, RenderTexture const* renderTexture, uint64 uavMipLevel) const = 0;
};