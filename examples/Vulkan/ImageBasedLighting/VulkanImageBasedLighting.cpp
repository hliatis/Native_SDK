/*!*********************************************************************************************************************
\File         VulkanImageBasedLighting.cpp
\Title        Introducing the PowerVR Framework
\Author       PowerVR by Imagination, Developer Technology Team
\Copyright    Copyright (c) Imagination Technologies Limited.
\brief        This example demonstrates how to use Physically based rendering using Metallic-Roughness work flow showcasing 2 scenes (helmet and sphere) with Image based lighting
			  (IBL). The Technique presented here is based on Epic Games publication http://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
***********************************************************************************************************************/

/*!
IBL Description
	Material: Metallic-Roughness
	============================
	- Albedo map: This is a raw color of the material. This map shouldn't contains any shading information like Ambient Occlusion which is
	very often baked in the diffuse map for phong model.
	It does not only influence the diffuse color, but also the specular color of the material as well.
	When the metallness is one(metallic material) the base color is the specular.

	- MetallicRoughness map: The metallic-roughness texture.
	The metalness values are sampled from the B channel and roughness values are sampled from the G channel, other channels are ignored.

	BRDF
	====
	*Diffuse BRDF: Lambertian diffuse
	f = Cdiff / PI
	Cdiff: Diffuse albedo of the material.

	*Specular BRDF: Cook-Torance
	f = D * F * G / (4 * (N.L) * (N.V));
	D: NDF (Normal Distribution function), It computes the distribution of the microfacets for the shaded surface
	F: Describes how light reflects and refracts at the intersection of two different media (most often in computer graphics : Air and the shaded surface)
	G: Defines the shadowing from the microfacets
	N.L:  is the dot product between the normal of the shaded surface and the light direction.
	N.V is the dot product between the normal of the shaded surface and the view direction.

	IBL workflow
	============
	IBL is one of the most common technique for implmenting global illumination. The idea is that using environmap as light source.

	IBL Diffuse:
	The application load/ generates a diffuse Irradiance map: This is normally done in offline but the code is left here for education
	purpose. Normally when lambert diffuse is used in games, it is the light color multiplied by the visibility factor( N dot L).
	But when using Indirectional lighting (IBL)  the visibility factor is not considered because the light is coming from every where.
	So the diffuse factor is the light color.
	All the pixels in the environment map is a light source, so when shading a point it has to be lit by many pixels from the environment map.
	Sampling multiple texels for shading a single point is not practical for realtime application. Therefore these samples are precomputed
	in the diffuse irradiance map. So at run time it would be a single texture fetch for the given reflection direction.

	IBL Specular & BRDF_LUT:
	Specular reflections looks shiny when the roughness values is low and it becames blurry when the roughness value is high.
	This is encoded in the specular irradiance texture.
	We use the same technique, Split-Sum-Approximation presented by Epics Games, each mip level of this image contains the environment map specular reflectance.
	Mip level 0 contains samples for roughness value 0, and the remaining miplevels get blurry for each mip level as the roughness value increases to 1.

	The samples encoded in this map is the result of the specular BRDF of the environment map. For each pixels in the environemt map,
	computes the Cook-Torrentz microfacet BRDF and stores those results.

	Using the mip map for storing blured images for each roughness value has one draw backs, Specular antialising.
	This happens for the level 0. Since we are using the mip map for different purpose, we can't use mipmapping technique
	to solve the aliasing artifact for high resoultion texture which is level0 of the specular irradiance map.
	Other mip map levels doesn'y have this issue as they are blured and low res.

	To solve this issue we use another texture for doing mipmaping for level 0 of the specular Irradiance map.
*/

#include "PVRShell/PVRShell.h"
#include "PVRUtils/PVRUtilsVk.h"
#include "PVRUtils/Vulkan/PBRUtilsVk.h"
#include "PVRCore/cameras/TPSCamera.h"
#include "PVRCore/textureio/TextureWriterPVR.h"
#include "PVRAssets/fileio/GltfReader.h"

// Content file names
// Shaders
const char VertShaderFileName[] = "VertShader.vsh.spv";
const char PBRFragShaderFileName[] = "PBRFragShader.fsh.spv";
const char SkyboxVertShaderFileName[] = "SkyboxVertShader.vsh.spv";
const char SkyboxFragShaderFileName[] = "SkyboxFragShader.fsh.spv";

// Models
const char HelmetModelFileName[] = "damagedHelmet.gltf";
const char SphereModelFileName[] = "sphere.pod";

uint32_t currentSkybox = 0;
// Textures
const std::string SkyboxTexFile[] = {
	"satara_night_scale_0.305_rgb9e5", //
	"misty_pines_rgb9e5", //
};

uint32_t numSkyBoxes = sizeof(SkyboxTexFile) / sizeof(SkyboxTexFile[0]);

const char BrdfLUTTexFile[] = "brdfLUT.pvr";

const uint32_t IrradianceMapDim = 64;
const uint32_t PrefilterEnvMapDim = 256;

const uint32_t NumSphereRows = 4;
const uint32_t NumSphereColumns = 6;
const uint32_t NumInstances = NumSphereRows * NumSphereColumns;

const float rotationSpeed = .01f;

float fov = 65.f;

const glm::vec3 lightDir = glm::normalize(glm::vec3(-0.5f, -0.5f, -0.5f));
const glm::vec3 lightColor = glm::vec3(0.f, 0.f, 0.f);

struct UBO
{
	pvr::utils::StructuredBufferView view;
	pvrvk::Buffer buffer;
};

enum class Models
{
	Helmet,
	Sphere,
	NumModels
};

class SkyBoxPass
{
public:
	void init(pvr::IAssetProvider& assetProvider, pvrvk::Device& device, pvrvk::DescriptorPool& descPool, pvrvk::CommandPool& commandPool, pvrvk::Queue& queue,
		const pvrvk::RenderPass& renderpass, const pvrvk::PipelineCache& pipelineCache, uint32_t numSwapchains, const pvrvk::Extent2D& viewportDim, const pvrvk::Sampler& sampler,
		pvr::utils::vma::Allocator& allocator)
	{
		// /// CREATE THE UBO that holds the information necessary to render the skybox /// //
		pvr::utils::StructuredMemoryDescription desc;
		desc.addElement("InvVPMatrix", pvr::GpuDatatypes::mat4x4);
		desc.addElement("EyePos", pvr::GpuDatatypes::vec4);
		desc.addElement("exposure", pvr::GpuDatatypes::Float);

		uboView.initDynamic(desc, numSwapchains, pvr::BufferUsageFlags::UniformBuffer,
			static_cast<uint32_t>(device->getPhysicalDevice()->getProperties().getLimits().getMinUniformBufferOffsetAlignment()));

		ubo = pvr::utils::createBuffer(device, uboView.getSize(), pvrvk::BufferUsageFlags::e_UNIFORM_BUFFER_BIT, pvrvk::MemoryPropertyFlags::e_HOST_VISIBLE_BIT,
			pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT | pvrvk::MemoryPropertyFlags::e_DEVICE_LOCAL_BIT, &allocator);
		uboView.pointToMappedMemory(ubo->getDeviceMemory()->getMappedData());

		// /// CREATE THE PIPELINE OBJECT FOR THE SKYBOX /// //
		// create skybox descriptor set layout
		pvrvk::DescriptorSetLayoutCreateInfo descSetLayoutInfo;
		descSetLayoutInfo.setBinding(0, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT);
		descSetLayoutInfo.setBinding(1, pvrvk::DescriptorType::e_UNIFORM_BUFFER_DYNAMIC, 1, pvrvk::ShaderStageFlags::e_VERTEX_BIT | pvrvk::ShaderStageFlags::e_FRAGMENT_BIT);

		pvrvk::DescriptorSetLayout descSetLayout = device->createDescriptorSetLayout(descSetLayoutInfo);

		pvrvk::PipelineLayoutCreateInfo pipelineLayoutInfo;
		pipelineLayoutInfo.setDescSetLayout(0, descSetLayout);

		pvrvk::PipelineLayout pipeLayout = device->createPipelineLayout(pipelineLayoutInfo);
		createPipeline(assetProvider, device, renderpass, viewportDim, pipeLayout);

		// /// CREATE THE SKYBOX DESCRIPTOR SET /// //
		descSet = descPool->allocateDescriptorSet(descSetLayout);

		setSkyboxImage(assetProvider, queue, commandPool, descPool, allocator, sampler);
	}

	void setSkyboxImage(pvr::IAssetProvider& assetProvider, pvrvk::Queue queue, pvrvk::CommandPool commandPool, pvrvk::DescriptorPool descPool,
		pvr::utils::vma::Allocator& allocator, const pvrvk::Sampler& sampler)
	{
		// /// LOAD THE SKYBOX TEXTURE /// //
		pvrvk::CommandBuffer cmdBuffer = commandPool->allocateCommandBuffer();
		pvrvk::Device device = commandPool->getDevice();

		cmdBuffer->begin();

		skyBoxMap = device->createImageView(pvrvk::ImageViewCreateInfo(pvr::utils::loadAndUploadImage(device, SkyboxTexFile[currentSkybox] + ".pvr", true, cmdBuffer, assetProvider,
			pvrvk::ImageUsageFlags::e_SAMPLED_BIT, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL, nullptr, &allocator, &allocator)));

		cmdBuffer->end();

		pvrvk::SubmitInfo submitInfo;
		submitInfo.commandBuffers = &cmdBuffer;
		submitInfo.numCommandBuffers = 1;
		queue->submit(&submitInfo, 1);
		queue->waitIdle();

		cmdBuffer->begin();

		pvrvk::WriteDescriptorSet writeDescSets[2];
		writeDescSets[0]
			.set(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, descSet, 0)
			.setImageInfo(0, pvrvk::DescriptorImageInfo(skyBoxMap, sampler, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));

		writeDescSets[1].set(pvrvk::DescriptorType::e_UNIFORM_BUFFER_DYNAMIC, descSet, 1).setBufferInfo(0, pvrvk::DescriptorBufferInfo(ubo, 0, uboView.getDynamicSliceSize()));

		device->updateDescriptorSets(writeDescSets, ARRAY_SIZE(writeDescSets), nullptr, 0);

		// Load (or generate) the other image based lighting files (diffuse/irradiance, specular/prefiltered)

		std::string diffuseMapFilename = SkyboxTexFile[currentSkybox] + "_Irradiance.pvr";
		std::string prefilteredMapFilename = SkyboxTexFile[currentSkybox] + "_Prefiltered.pvr";

		irradianceMap = pvr::utils::loadAndUploadImageAndView(device, diffuseMapFilename.c_str(), true, cmdBuffer, assetProvider, pvrvk::ImageUsageFlags::e_SAMPLED_BIT,
			pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL, nullptr, &allocator, &allocator);
		prefilteredMap = pvr::utils::loadAndUploadImageAndView(device, prefilteredMapFilename.c_str(), true, cmdBuffer, assetProvider, pvrvk::ImageUsageFlags::e_SAMPLED_BIT,
			pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL, nullptr, &allocator, &allocator);

		numPrefilteredMipLevels = prefilteredMap->getImage()->getNumMipLevels();

		cmdBuffer->end();
		queue->submit(&submitInfo, 1);
		queue->waitIdle();
	}

	uint32_t getNumPrefilteredMipLevels() const
	{
		return numPrefilteredMipLevels;
	}

	pvrvk::ImageView getDiffuseIrradianceMap()
	{
		return irradianceMap;
	}

	pvrvk::ImageView getPrefilteredMap()
	{
		return prefilteredMap;
	}

	pvrvk::ImageView getPrefilteredMipMap()
	{
		return skyBoxMap;
	}

	/// <summary>Update Per frame.</summary>
	/// <param name="swapchainIndex">current swapchain index</param>
	/// <param name="invViewProj">inverse view projection matrix.</param>
	/// <param name="eyePos">camera position</param>
	void update(uint32_t swapchainIndex, const glm::mat4& invViewProj, const glm::vec3& eyePos, float exposure)
	{
		uboView.getElement(0, 0, swapchainIndex).setValue(invViewProj);
		uboView.getElement(1, 0, swapchainIndex).setValue(glm::vec4(eyePos, 0.0f));
		uboView.getElement(2, 0, swapchainIndex).setValue(exposure);
		if (uint32_t(ubo->getDeviceMemory()->getMemoryFlags() & pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT) == 0)
		{
			ubo->getDeviceMemory()->flushRange(uboView.getDynamicSliceOffset(swapchainIndex), uboView.getDynamicSliceSize());
		}
	}

	/// <summary>Record commands.</summary>
	/// <param name="cmdBuffer">recording commandbuffer</param>
	/// <param name="swapchainIndex">swapchain index.</param>
	void recordCommands(pvrvk::CommandBuffer& cmdBuffer, uint32_t swapchainIndex)
	{
		cmdBuffer->bindPipeline(pipeline);
		uint32_t offset = uboView.getDynamicSliceOffset(swapchainIndex);
		cmdBuffer->bindDescriptorSet(pvrvk::PipelineBindPoint::e_GRAPHICS, pipeline->getPipelineLayout(), 0, descSet, &offset, 1);

		cmdBuffer->draw(0, 6, 0);
	}

private:
	/// <summary>Create uniform buffer objects</summary>
	/// <param name="device">The device the vulkan resources allocated from.</param>
	/// <param name="numSwapchains">Number of swapchains.</param>
	/// <param name="allocator">buffer memory allocator.</param>
	void createUbo(pvrvk::Device& device, uint32_t numSwapchains, pvr::utils::vma::Allocator& allocator) {}

	void createPipeline(pvr::IAssetProvider& assetProvider, pvrvk::Device& device, const pvrvk::RenderPass& renderpass, const pvrvk::Extent2D& viewportDim,
		const pvrvk::PipelineLayout& pipelineLayout)
	{
		pvrvk::GraphicsPipelineCreateInfo pipeInfo;

		// on screen renderpass
		pipeInfo.renderPass = renderpass;

		pipeInfo.vertexShader.setShader(device->createShaderModule(pvrvk::ShaderModuleCreateInfo(assetProvider.getAssetStream(SkyboxVertShaderFileName)->readToEnd<uint32_t>())));
		pipeInfo.fragmentShader.setShader(device->createShaderModule(pvrvk::ShaderModuleCreateInfo(assetProvider.getAssetStream(SkyboxFragShaderFileName)->readToEnd<uint32_t>())));

		pipeInfo.pipelineLayout = pipelineLayout;

		// depth stencil state
		pipeInfo.depthStencil.enableDepthWrite(false);
		pipeInfo.depthStencil.enableDepthTest(false);

		// rasterizer state
		pipeInfo.rasterizer.setCullMode(pvrvk::CullModeFlags::e_BACK_BIT);

		// blend state
		pipeInfo.colorBlend.setAttachmentState(0, pvrvk::PipelineColorBlendAttachmentState());

		// input assembler
		pipeInfo.inputAssembler.setPrimitiveTopology(pvrvk::PrimitiveTopology::e_TRIANGLE_LIST);

		// vertex attributes and bindings
		pipeInfo.vertexInput.clear();

		pipeInfo.viewport.setViewportAndScissor(0, pvrvk::Viewport(0.0f, 0.0f, static_cast<float>(viewportDim.getWidth()), static_cast<float>(viewportDim.getHeight())),
			pvrvk::Rect2D(0, 0, viewportDim.getWidth(), viewportDim.getHeight()));

		pipeline = device->createGraphicsPipeline(pipeInfo);
	}

	pvrvk::GraphicsPipeline pipeline;
	pvrvk::ImageView skyBoxMap;
	pvrvk::ImageView irradianceMap, prefilteredMap;
	pvrvk::DescriptorSet descSet;
	pvr::utils::StructuredBufferView uboView;
	pvrvk::Buffer ubo;
	uint32_t numPrefilteredMipLevels;
};

class SpherePass
{
public:
	/// <summary>initialise the sphere's pipeline</summary>
	/// <param name="device">Device for the resource to create from</param>
	/// <param name="assetProvider">Asset provider for loading assets from disk.</param>
	/// <param name="basePipeline">Handle to base pipeline</param>
	/// <param name="uploadCmdBuffer">initial image uploading command buffer</param>
	/// <param name="requireSubmission">Returns boolean flag whether the command buffer need submission.</param>
	void init(pvr::IAssetProvider& assetProvider, pvrvk::Device& device, const pvrvk::GraphicsPipeline& basePipeline, const pvrvk::PipelineCache& pipelineCache,
		pvr::utils::vma::Allocator& allocator, pvrvk::CommandBuffer& uploadCmdBuffer, bool& requireSubmission)
	{
		model = pvr::assets::Model::createWithReader(pvr::assets::PODReader(assetProvider.getAssetStream(SphereModelFileName)));

		pvr::utils::appendSingleBuffersFromModel(device, *model, vbos, ibos, uploadCmdBuffer, requireSubmission, &allocator);

		createPipeline(assetProvider, device, basePipeline, pipelineCache);
	}

	/// <summary>Record commands for rendering the sphere model</summary>
	/// <param name="cmdBuffer">A command buffer to which sphere commands will be added</param>
	void recordCommands(pvrvk::CommandBuffer& cmdBuffer)
	{
		cmdBuffer->bindPipeline(pipeline);
		uint32_t one = 1;
		cmdBuffer->pushConstants(pipeline->getPipelineLayout(), pvrvk::ShaderStageFlags::e_VERTEX_BIT | pvrvk::ShaderStageFlags::e_FRAGMENT_BIT, 0,
			static_cast<uint32_t>(pvr::getSize(pvr::GpuDatatypes::uinteger)), &one);
		cmdBuffer->pushConstants(pipeline->getPipelineLayout(), pvrvk::ShaderStageFlags::e_VERTEX_BIT | pvrvk::ShaderStageFlags::e_FRAGMENT_BIT,
			static_cast<uint32_t>(pvr::getSize(pvr::GpuDatatypes::uinteger)), static_cast<uint32_t>(pvr::getSize(pvr::GpuDatatypes::uinteger)), &one);
		for (uint32_t i = 0; i < model->getNumMeshNodes(); ++i)
		{
			pvr::assets::Node& node = model->getMeshNode(i);
			pvr::assets::Mesh& mesh = model->getMesh(static_cast<uint32_t>(node.getObjectId()));

			cmdBuffer->bindVertexBuffer(vbos[i], 0, 0);
			cmdBuffer->bindIndexBuffer(ibos[i], 0, mesh.getFaces().getDataType() == pvr::IndexType::IndexType16Bit ? pvrvk::IndexType::e_UINT16 : pvrvk::IndexType::e_UINT32);
			cmdBuffer->drawIndexed(0, mesh.getNumFaces() * 3, 0, 0, NumInstances);
		}
	}

private:
	void createPipeline(pvr::IAssetProvider& assetProvider, pvrvk::Device& device, const pvrvk::GraphicsPipeline& basePipeline, const pvrvk::PipelineCache& pipelineCache)
	{
		pvrvk::GraphicsPipelineCreateInfo pipeDesc = basePipeline->getCreateInfo();
		pipeDesc.basePipeline = basePipeline;
		pvr::utils::VertexBindings bindingName[] = { { "POSITION", 0 }, { "NORMAL", 1 } };

		pipeDesc.vertexInput.clear();
		pvr::utils::populateInputAssemblyFromMesh(model->getMesh(0), bindingName, ARRAY_SIZE(bindingName), pipeDesc.vertexInput, pipeDesc.inputAssembler);
		pipeDesc.vertexInput.addInputAttribute(pvrvk::VertexInputAttributeDescription(2, 0, pvrvk::Format::e_R32G32_SFLOAT, 0)); // THIS WILL NOT BE USED BUT MUST BE PROVIDED
		pipeDesc.vertexInput.addInputAttribute(pvrvk::VertexInputAttributeDescription(3, 0, pvrvk::Format::e_R32G32B32A32_SFLOAT, 0)); // THIS WILL NOT BE USED BUT MUST BE PROVIDED

		pipeDesc.vertexShader = device->createShaderModule(pvrvk::ShaderModuleCreateInfo(assetProvider.getAssetStream(VertShaderFileName)->readToEnd<uint32_t>()));
		pipeDesc.fragmentShader = device->createShaderModule(pvrvk::ShaderModuleCreateInfo(assetProvider.getAssetStream(PBRFragShaderFileName)->readToEnd<uint32_t>()));

		// SET SPECIALIZATION CONSTANTS
		static VkBool32 shaderConstantHasTextures = false;
		pipeDesc.fragmentShader.setShaderConstant(0, pvrvk::ShaderConstantInfo(0, &shaderConstantHasTextures, sizeof(VkBool32)));
		pipeDesc.vertexShader.setShaderConstant(0, pvrvk::ShaderConstantInfo(0, &shaderConstantHasTextures, sizeof(VkBool32)));

		pipeline = device->createGraphicsPipeline(pipeDesc, pipelineCache);
	}

	pvr::assets::ModelHandle model;
	std::vector<pvrvk::Buffer> vbos;
	std::vector<pvrvk::Buffer> ibos;
	pvrvk::GraphicsPipeline pipeline;
};

class HelmetPass
{
public:
	void init(pvr::IAssetProvider& assetProvider, pvrvk::Device& device, const pvrvk::Framebuffer& framebuffer, const pvrvk::PipelineLayout& pipelineLayout,
		const pvrvk::PipelineCache& pipelineCache, pvr::utils::vma::Allocator& allocator, pvrvk::CommandBuffer& uploadCmdBuffer, bool requireSubmission)
	{
		model = pvr::assets::Model::createWithReader(pvr::assets::GltfReader(assetProvider.getAssetStream(HelmetModelFileName), assetProvider));

		// create the vbo and ibo for the meshes.
		vbos.resize(model->getNumMeshes());
		ibos.resize(model->getNumMeshes());

		pvr::utils::createSingleBuffersFromMesh(device, model->getMesh(0), vbos[0], ibos[0], uploadCmdBuffer, requireSubmission, &allocator);

		// Load the texture
		loadTextures(assetProvider, device, uploadCmdBuffer, allocator);

		createPipeline(assetProvider, device, framebuffer, pipelineLayout, pipelineCache);
	}

	const pvrvk::GraphicsPipeline& getPipeline()
	{
		return pipeline;
	}

	pvr::assets::ModelHandle& getModel()
	{
		return model;
	}

	const pvrvk::ImageView& getAlbedoMap()
	{
		return images[0];
	}

	const pvrvk::ImageView& getOcclusionMetallicRoughnessMap()
	{
		return images[1];
	}

	const pvrvk::ImageView& getNormalMap()
	{
		return images[2];
	}

	const pvrvk::ImageView& getEmissiveMap()
	{
		return images[3];
	}

	void recordCommands(pvrvk::CommandBuffer& cmd)
	{
		cmd->bindPipeline(pipeline);

		const uint32_t numMeshes = model->getNumMeshes();
		// set the model matrix and material id.
		uint32_t zero = 0;
		cmd->pushConstants(pipeline->getPipelineLayout(), pvrvk::ShaderStageFlags::e_VERTEX_BIT | pvrvk::ShaderStageFlags::e_FRAGMENT_BIT, 0,
			static_cast<uint32_t>(pvr::getSize(pvr::GpuDatatypes::uinteger)), &zero);
		cmd->pushConstants(pipeline->getPipelineLayout(), pvrvk::ShaderStageFlags::e_VERTEX_BIT | pvrvk::ShaderStageFlags::e_FRAGMENT_BIT,
			static_cast<uint32_t>(pvr::getSize(pvr::GpuDatatypes::uinteger)), static_cast<uint32_t>(pvr::getSize(pvr::GpuDatatypes::uinteger)), &zero);

		for (uint32_t j = 0; j < numMeshes; ++j)
		{
			const pvr::assets::Mesh& mesh = model->getMesh(j);
			// find the texture descriptor set which matches the current material

			// bind the vbo and ibos for the current mesh node
			cmd->bindVertexBuffer(vbos[j], 0, 0);

			cmd->bindIndexBuffer(ibos[j], 0, mesh.getFaces().getDataType() == pvr::IndexType::IndexType16Bit ? pvrvk::IndexType::e_UINT16 : pvrvk::IndexType::e_UINT32);

			// draws
			cmd->drawIndexed(0, mesh.getNumFaces() * 3);
		}
	}

private:
	void createPipeline(pvr::IAssetProvider& assetProvider, pvrvk::Device& device, const pvrvk::Framebuffer& framebuffer, const pvrvk::PipelineLayout& pipelineLayout,
		const pvrvk::PipelineCache& pipelineCache)
	{
		pvrvk::GraphicsPipelineCreateInfo pipeDesc;
		pipeDesc.colorBlend.setAttachmentState(0, pvrvk::PipelineColorBlendAttachmentState());
		pvr::utils::VertexBindings bindingName[] = { { "POSITION", 0 }, { "NORMAL", 1 }, { "UV0", 2 }, { "TANGENT", 3 } };

		pvr::utils::populateViewportStateCreateInfo(framebuffer, pipeDesc.viewport);
		pvr::utils::populateInputAssemblyFromMesh(getModel()->getMesh(0), bindingName, ARRAY_SIZE(bindingName), pipeDesc.vertexInput, pipeDesc.inputAssembler);

		pipeDesc.vertexShader.setShader(device->createShaderModule(pvrvk::ShaderModuleCreateInfo(assetProvider.getAssetStream(VertShaderFileName)->readToEnd<uint32_t>())));
		pipeDesc.fragmentShader.setShader(device->createShaderModule(pvrvk::ShaderModuleCreateInfo(assetProvider.getAssetStream(PBRFragShaderFileName)->readToEnd<uint32_t>())));

		static VkBool32 shaderConstantHasTextures = 1;
		pipeDesc.vertexShader.setShaderConstant(0, pvrvk::ShaderConstantInfo(0, &shaderConstantHasTextures, sizeof(VkBool32)));
		pipeDesc.fragmentShader.setShaderConstant(0, pvrvk::ShaderConstantInfo(0, &shaderConstantHasTextures, sizeof(VkBool32)));

		pipeDesc.renderPass = framebuffer->getRenderPass();
		pipeDesc.depthStencil.enableDepthTest(true);
		pipeDesc.inputAssembler.setPrimitiveTopology(pvrvk::PrimitiveTopology::e_TRIANGLE_LIST);
		pipeDesc.depthStencil.setDepthCompareFunc(pvrvk::CompareOp::e_LESS);
		pipeDesc.depthStencil.enableDepthWrite(true);
		pipeDesc.rasterizer.setCullMode(pvrvk::CullModeFlags::e_BACK_BIT).setFrontFaceWinding(pvrvk::FrontFace::e_COUNTER_CLOCKWISE);
		pipeDesc.subpass = 0;

		pipeDesc.pipelineLayout = pipelineLayout;

		pipeline = device->createGraphicsPipeline(pipeDesc);
	}

	void loadTextures(pvr::IAssetProvider& assetProvider, pvrvk::Device& device, pvrvk::CommandBuffer& uploadCmdBuffer, pvr::utils::vma::Allocator& allocator)
	{
		for (uint32_t i = 0; i < model->getNumTextures(); ++i)
		{
			pvr::Stream::ptr_type stream = assetProvider.getAssetStream(model->getTexture(i).getName());
			pvr::Texture tex = pvr::textureLoad(stream, pvr::TextureFileFormat::PVR);
			images.push_back(pvr::utils::uploadImageAndView(device, tex, true, uploadCmdBuffer, pvrvk::ImageUsageFlags::e_SAMPLED_BIT,
				pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL, &allocator, &allocator, pvr::utils::vma::AllocationCreateFlags::e_DEDICATED_MEMORY_BIT));
		}
	}

	std::vector<pvrvk::ImageView> images;
	std::vector<pvrvk::Buffer> vbos;
	std::vector<pvrvk::Buffer> ibos;
	pvr::assets::ModelHandle model;
	pvrvk::GraphicsPipeline pipeline;
};

/*!*********************************************************************************************************************
 Class implementing the pvr::Shell functions.
***********************************************************************************************************************/
class VulkanImageBasedLighting : public pvr::Shell
{
	typedef std::pair<int32_t, pvrvk::DescriptorSet> MaterialDescSet;

	enum DescSetIndex
	{
		PerFrame,
		Model,
		Material,
	};

	struct DeviceResources
	{
		pvrvk::Instance instance;
		pvr::utils::DebugUtilsCallbacks debugUtilsCallbacks;
		pvrvk::Surface surface;
		pvrvk::Device device;
		pvrvk::Swapchain swapchain;
		pvr::utils::vma::Allocator vmaAllocator;
		pvr::Multi<pvrvk::ImageView> depthStencilImages;
		pvrvk::Queue queue;

		pvrvk::CommandPool commandPool;
		pvrvk::DescriptorPool descriptorPool;

		pvrvk::Semaphore imageAcquiredSemaphores[static_cast<uint32_t>(pvrvk::FrameworkCaps::MaxSwapChains)];
		pvrvk::Semaphore presentationSemaphores[static_cast<uint32_t>(pvrvk::FrameworkCaps::MaxSwapChains)];
		pvrvk::Fence perFrameResourcesFences[static_cast<uint32_t>(pvrvk::FrameworkCaps::MaxSwapChains)];

		// the framebuffer used in the demo
		pvr::Multi<pvrvk::Framebuffer> onScreenFramebuffer;

		// main command buffer used to store rendering commands
		pvr::Multi<pvrvk::CommandBuffer> commandBuffers;

		// Pipeline cache
		pvrvk::PipelineCache pipelineCache;

		// descriptor sets
		pvrvk::DescriptorSet descSets[3];

		// structured memory views
		UBO uboPerFrame, uboLights, uboMaterial, uboWorld;

		// samplers
		pvrvk::Sampler samplerBilinear, samplerTrilinear, samplerTrilinearLodClamped;

		// descriptor set layouts
		pvrvk::DescriptorSetLayout descSetLayouts[3];

		pvrvk::PipelineLayout pipelineLayout;

		pvrvk::ImageView brdfLUT;

		pvr::ui::UIRenderer uiRenderer;

		SkyBoxPass skyBoxPass;
		HelmetPass helmetPass;
		SpherePass spherePass;

		~DeviceResources()
		{
			if (device)
			{
				device->waitIdle();
			}
			uint32_t l = swapchain->getSwapchainLength();
			for (uint32_t i = 0; i < l; ++i)
			{
				if (perFrameResourcesFences[i])
					perFrameResourcesFences[i]->wait();
			}
		}
	};

	std::unique_ptr<DeviceResources> _deviceResources;

	bool _updateCommands[static_cast<uint32_t>(pvrvk::FrameworkCaps::MaxSwapChains)];
	bool _updateDescriptors = false;

	// Projection and Model View matrices
	glm::mat4 _projMtx;
	// Variables to handle the animation in a time-based manner
	float _frame;
	uint32_t _frameId;

	pvr::TPSOrbitCamera _camera;
	Models _currentModel = Models::Helmet;
	bool _pause = false;
	float exposure = 1.f;

public:
	virtual pvr::Result initApplication();
	virtual pvr::Result initView();
	virtual pvr::Result releaseView();
	virtual pvr::Result quitApplication();
	virtual pvr::Result renderFrame();

	void createDescriptorSetLayouts();
	void createUbos();
	void updateDescriptors();
	void recordCommandBuffers(uint32_t swapIndex);
	void createPipelineLayout();

	virtual void eventMappedInput(pvr::SimplifiedInput action)
	{
		float oldexposure = exposure;
		switch (action)
		{
		case pvr::SimplifiedInput::Action1: {
			_pause = !_pause;
			break;
		}
		case pvr::SimplifiedInput::Action2: {
			uint32_t currentModel = static_cast<uint32_t>(_currentModel);
			currentModel += 1;
			currentModel = (currentModel + static_cast<uint32_t>(Models::NumModels)) % static_cast<uint32_t>(Models::NumModels);
			_currentModel = static_cast<Models>(currentModel);
			memset(_updateCommands, 1, sizeof(_updateCommands));
			break;
		}
		case pvr::SimplifiedInput::Action3: {
			(++currentSkybox) %= numSkyBoxes;
			_deviceResources->skyBoxPass.setSkyboxImage(*this, _deviceResources->queue, _deviceResources->commandPool, _deviceResources->descriptorPool,
				_deviceResources->vmaAllocator, _deviceResources->samplerTrilinear);
			memset(_updateCommands, 1, sizeof(_updateCommands));
			_updateDescriptors = true;
			break;
		}
		case pvr::SimplifiedInput::Left: {
			exposure *= .75;
			if (oldexposure > 1.f && exposure < 1.f)
				exposure = 1.f;
			break;
		}
		case pvr::SimplifiedInput::Right: {
			exposure *= 1.25;
			if (oldexposure < 1.f && exposure > 1.f)
				exposure = 1.f;
			break;
		}
		case pvr::SimplifiedInput::ActionClose: {
			this->exitShell();
			break;
		}
		default:
			break;
		}
	}
};

/// <summary>
/// Code in initApplication() will be called by Shell once per run, before the rendering context is created.
/// Used to initialize variables that are not dependent on it (e.g. external modules, loading meshes, etc.). If the rendering
/// context is lost, initApplication() will not be called again.</summary>
pvr::Result VulkanImageBasedLighting::initApplication()
{
	_frame = 0;
	_frameId = 0;
	setBackBufferColorspace(pvr::ColorSpace::lRGB);
	return pvr::Result::Success;
}

/// <summary>Code in quitApplication() will be called by Shell once per run, just before exiting the program.
/// quitApplication() will not be called every time the rendering context is lost, only before application exit.</summary>
/// <returns>Result::Success if no error occurred.</returns>
pvr::Result VulkanImageBasedLighting::quitApplication()
{
	return pvr::Result::Success;
}

/// <summary>Code in initView() will be called by Shell upon initialization or after a change in the rendering context.
/// Used to initialize variables that are dependent on the rendering context(e.g.textures, vertex buffers, etc.)</summary>
/// <returns>Result::Success if no error occurred.</returns>
pvr::Result VulkanImageBasedLighting::initView()
{
	_deviceResources = std::unique_ptr<DeviceResources>(new DeviceResources());

	// Create vulkan instance and surface
	_deviceResources->instance = pvr::utils::createInstance(this->getApplicationName());
	_deviceResources->surface = pvr::utils::createSurface(_deviceResources->instance, _deviceResources->instance->getPhysicalDevice(0), this->getWindow(), this->getDisplay());

	// Create a default set of debug utils messengers or debug callbacks using either VK_EXT_debug_utils or VK_EXT_debug_report respectively
	_deviceResources->debugUtilsCallbacks = pvr::utils::createDebugUtilsCallbacks(_deviceResources->instance);

	pvrvk::PhysicalDevice physicalDevice = _deviceResources->instance->getPhysicalDevice(0);

	// Populate queue for rendering and transfer operation
	const pvr::utils::QueuePopulateInfo queuePopulateInfo = { pvrvk::QueueFlags::e_GRAPHICS_BIT, _deviceResources->surface };

	// Create the device and queue
	pvr::utils::QueueAccessInfo queueAccessInfo;
	_deviceResources->device = pvr::utils::createDeviceAndQueues(physicalDevice, &queuePopulateInfo, 1, &queueAccessInfo);

	// Get the queue
	_deviceResources->queue = _deviceResources->device->getQueue(queueAccessInfo.familyId, queueAccessInfo.queueId);

	// validate the supported swapchain image usage for src trasfer option for capturing screenshots.
	pvrvk::SurfaceCapabilitiesKHR surfaceCapabilities = physicalDevice->getSurfaceCapabilities(_deviceResources->surface);
	pvrvk::ImageUsageFlags swapchainImageUsage = pvrvk::ImageUsageFlags::e_COLOR_ATTACHMENT_BIT;
	if (pvr::utils::isImageUsageSupportedBySurface(surfaceCapabilities, pvrvk::ImageUsageFlags::e_TRANSFER_SRC_BIT))
	{
		swapchainImageUsage |= pvrvk::ImageUsageFlags::e_TRANSFER_SRC_BIT; // Transfer operation supported.
	}

	// initialse the vma allocator
	_deviceResources->vmaAllocator = pvr::utils::vma::createAllocator(pvr::utils::vma::AllocatorCreateInfo(_deviceResources->device));

	// Create the swapchain and depth-stencil image views
	pvr::utils::createSwapchainAndDepthStencilImageAndViews(_deviceResources->device, _deviceResources->surface, getDisplayAttributes(), _deviceResources->swapchain,
		_deviceResources->depthStencilImages, swapchainImageUsage, pvrvk::ImageUsageFlags::e_DEPTH_STENCIL_ATTACHMENT_BIT | pvrvk::ImageUsageFlags::e_TRANSIENT_ATTACHMENT_BIT,
		&_deviceResources->vmaAllocator);

	// Create the framebuffer
	pvrvk::RenderPass rp;
	pvr::utils::createOnscreenFramebufferAndRenderPass(_deviceResources->swapchain, &_deviceResources->depthStencilImages[0], _deviceResources->onScreenFramebuffer, rp);

	// Create the Commandpool & Descriptorpool
	_deviceResources->commandPool =
		_deviceResources->device->createCommandPool(pvrvk::CommandPoolCreateInfo(queueAccessInfo.familyId, pvrvk::CommandPoolCreateFlags::e_RESET_COMMAND_BUFFER_BIT));
	if (!_deviceResources->commandPool)
	{
		return pvr::Result::UnknownError;
	}

	_deviceResources->descriptorPool = _deviceResources->device->createDescriptorPool(pvrvk::DescriptorPoolCreateInfo()
																						  .addDescriptorInfo(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 16)
																						  .addDescriptorInfo(pvrvk::DescriptorType::e_UNIFORM_BUFFER_DYNAMIC, 16)
																						  .addDescriptorInfo(pvrvk::DescriptorType::e_UNIFORM_BUFFER, 16)
																						  .addDescriptorInfo(pvrvk::DescriptorType::e_STORAGE_IMAGE, 2)
																						  .setMaxDescriptorSets(16));

	if (!_deviceResources->descriptorPool)
	{
		return pvr::Result::UnknownError;
	}

	// Create synchronization objects and commandbuffers
	for (uint32_t i = 0; i < _deviceResources->swapchain->getSwapchainLength(); ++i)
	{
		_deviceResources->presentationSemaphores[i] = _deviceResources->device->createSemaphore();
		_deviceResources->imageAcquiredSemaphores[i] = _deviceResources->device->createSemaphore();
		_deviceResources->perFrameResourcesFences[i] = _deviceResources->device->createFence(pvrvk::FenceCreateFlags::e_SIGNALED_BIT);
		_deviceResources->commandBuffers[i] = _deviceResources->commandPool->allocateCommandBuffer();
		_updateCommands[i] = true;
	}

	// Create the pipeline cache
	_deviceResources->pipelineCache = _deviceResources->device->createPipelineCache();

	// create the sampler object
	pvrvk::SamplerCreateInfo samplerInfo;
	samplerInfo.minFilter = samplerInfo.magFilter = pvrvk::Filter::e_LINEAR;
	samplerInfo.mipMapMode = pvrvk::SamplerMipmapMode::e_NEAREST;
	samplerInfo.wrapModeU = samplerInfo.wrapModeV = samplerInfo.wrapModeW = pvrvk::SamplerAddressMode::e_CLAMP_TO_EDGE;
	_deviceResources->samplerBilinear = _deviceResources->device->createSampler(samplerInfo);

	// trilinear
	samplerInfo.mipMapMode = pvrvk::SamplerMipmapMode::e_LINEAR;
	_deviceResources->samplerTrilinear = _deviceResources->device->createSampler(samplerInfo);

	// trilinear with max lod clamping
	samplerInfo.lodMinimum = 2.f;
	_deviceResources->samplerTrilinearLodClamped = _deviceResources->device->createSampler(samplerInfo);

	_deviceResources->commandBuffers[0]->begin();

	// BRDF is of course pre-generated. To generate it
	// pvr::Texture brdflut = pvr::utils::generateCookTorranceBRDFLUT();
	// pvr::assetWriters::AssetWriterPVR(pvr::FileStream::createFileStream(BrdfLUTTexFile, "w")).writeAsset(brdflut);
	// See Calculating Assets example

	_deviceResources->brdfLUT = _deviceResources->device->createImageView(
		pvrvk::ImageViewCreateInfo(pvr::utils::loadAndUploadImage(_deviceResources->device, BrdfLUTTexFile, true, _deviceResources->commandBuffers[0], *this,
			pvrvk::ImageUsageFlags::e_SAMPLED_BIT, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL, nullptr, &_deviceResources->vmaAllocator, &_deviceResources->vmaAllocator)));

	createDescriptorSetLayouts();
	createPipelineLayout();

	// Create Descriptor Sets
	_deviceResources->descSets[0] = _deviceResources->descriptorPool->allocateDescriptorSet(_deviceResources->descSetLayouts[0]);
	_deviceResources->descSets[1] = _deviceResources->descriptorPool->allocateDescriptorSet(_deviceResources->descSetLayouts[1]);
	_deviceResources->descSets[2] = _deviceResources->descriptorPool->allocateDescriptorSet(_deviceResources->descSetLayouts[2]);

	bool requireSubmission = false;

	_deviceResources->skyBoxPass.init(*this, _deviceResources->device, _deviceResources->descriptorPool, _deviceResources->commandPool, _deviceResources->queue,
		_deviceResources->onScreenFramebuffer[0]->getRenderPass(), _deviceResources->pipelineCache, _deviceResources->swapchain->getSwapchainLength(),
		pvrvk::Extent2D(getWidth(), getHeight()), _deviceResources->samplerTrilinear, _deviceResources->vmaAllocator);

	_deviceResources->helmetPass.init(*this, _deviceResources->device, _deviceResources->onScreenFramebuffer[0], _deviceResources->pipelineLayout, _deviceResources->pipelineCache,
		_deviceResources->vmaAllocator, _deviceResources->commandBuffers[0], requireSubmission);

	_deviceResources->spherePass.init(*this, _deviceResources->device, _deviceResources->helmetPass.getPipeline(), _deviceResources->pipelineCache, _deviceResources->vmaAllocator,
		_deviceResources->commandBuffers[0], requireSubmission);

	createUbos();

	updateDescriptors(); // Actually populate the data

	_deviceResources->uiRenderer.init(getWidth(), getHeight(), isFullScreen(), _deviceResources->onScreenFramebuffer[0]->getRenderPass(), 0,
		getBackBufferColorspace() == pvr::ColorSpace::sRGB, _deviceResources->commandPool, _deviceResources->queue);

	_deviceResources->commandBuffers[0]->end();

	pvrvk::SubmitInfo submitInfo;
	submitInfo.commandBuffers = &_deviceResources->commandBuffers[0];
	submitInfo.numCommandBuffers = 1;

	// submit the queue and wait for it to become idle
	_deviceResources->queue->submit(&submitInfo, 1);
	_deviceResources->queue->waitIdle();
	_deviceResources->commandBuffers[0]->reset(pvrvk::CommandBufferResetFlags::e_RELEASE_RESOURCES_BIT);

	// Calculates the projection matrix
	bool isRotated = this->isScreenRotated() && this->isFullScreen();
	if (isRotated)
	{
		_projMtx = pvr::math::perspective(
			pvr::Api::Vulkan, glm::radians(fov), static_cast<float>(this->getHeight()) / static_cast<float>(this->getWidth()), 1.f, 2000.f, glm::pi<float>() * .5f);
	}
	else
	{
		_projMtx = pvr::math::perspective(pvr::Api::Vulkan, glm::radians(fov), static_cast<float>(this->getWidth()) / static_cast<float>(this->getHeight()), 1.f, 2000.f);
	}

	_deviceResources->uiRenderer.getDefaultTitle()->setText("ImageBasedLighting").commitUpdates();
	_deviceResources->uiRenderer.getDefaultControls()->setText("Action 1: Pause\n"
															   "Action 2: Change model\n"
															   "Action 3: Change scene\n");
	_deviceResources->uiRenderer.getDefaultControls()->commitUpdates();

	// setup the camera
	_camera.setDistanceFromTarget(50.f);
	_camera.setInclination(10.f);

	return pvr::Result::Success;
}

/// <summary>Code in releaseView() will be called by Shell when the application quits or before a change in the rendering context.</summary>
/// <returns>Result::Success if no error occurred.</returns>
pvr::Result VulkanImageBasedLighting::releaseView()
{
	_deviceResources.reset();
	return pvr::Result::Success;
}

/// <summary>Main rendering loop function of the program. The shell will call this function every frame</summary>
/// <returns>Result::Success if no error occurred.</summary>
pvr::Result VulkanImageBasedLighting::renderFrame()
{
	static float emissiveScale = 0.0f;
	static float emissiveStrength = 1.;

	_deviceResources->swapchain->acquireNextImage(uint64_t(-1), _deviceResources->imageAcquiredSemaphores[_frameId]);

	const uint32_t swapchainIndex = _deviceResources->swapchain->getSwapchainIndex();

	_deviceResources->perFrameResourcesFences[swapchainIndex]->wait();
	_deviceResources->perFrameResourcesFences[swapchainIndex]->reset();

	if (_updateDescriptors)
	{
		updateDescriptors();
		_updateDescriptors = false;
	}

	// Re-record the commandbuffer if the model has changed.
	if (_updateCommands[swapchainIndex])
	{
		recordCommandBuffers(swapchainIndex);
		_updateCommands[swapchainIndex] = false;
	}

	emissiveStrength += .15f;
	if (emissiveStrength >= glm::pi<float>())
	{
		emissiveStrength = 0.0f;
	}

	emissiveScale = std::abs(glm::cos(emissiveStrength)) + .75f;

	if (!_pause)
	{
		_camera.addAzimuth(getFrameTime() * rotationSpeed);
	}

	if (this->isKeyPressed(pvr::Keys::A))
	{
		_camera.addAzimuth(getFrameTime() * -.1f);
	}
	if (this->isKeyPressed(pvr::Keys::D))
	{
		_camera.addAzimuth(getFrameTime() * .1f);
	}

	if (this->isKeyPressed(pvr::Keys::W))
	{
		_camera.addInclination(getFrameTime() * .1f);
	}
	if (this->isKeyPressed(pvr::Keys::S))
	{
		_camera.addInclination(getFrameTime() * -.1f);
	}

	glm::mat4 viewMtx = _camera.getViewMatrix();
	glm::vec3 cameraPos = _camera.getCameraPosition();

	// update the matrix uniform buffer
	{
		// only update the current swapchain ubo
		const glm::mat4 tempMtx = _projMtx * viewMtx;
		_deviceResources->uboPerFrame.view.getElement(0, 0, swapchainIndex).setValue(viewMtx); // view matrix
		_deviceResources->uboPerFrame.view.getElement(1, 0, swapchainIndex).setValue(tempMtx); // view proj
		_deviceResources->uboPerFrame.view.getElement(2, 0, swapchainIndex).setValue(cameraPos); // camera position.
		_deviceResources->uboPerFrame.view.getElement(3, 0, swapchainIndex).setValue(emissiveScale);
		_deviceResources->uboPerFrame.view.getElement(4, 0, swapchainIndex).setValue(exposure);

		// flush if the buffer memory doesn't support host coherent.
		if (uint32_t(_deviceResources->uboPerFrame.buffer->getDeviceMemory()->getMemoryFlags() & pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT) == 0)
		{
			_deviceResources->uboPerFrame.buffer->getDeviceMemory()->flushRange(
				_deviceResources->uboPerFrame.view.getDynamicSliceOffset(swapchainIndex), _deviceResources->uboPerFrame.view.getDynamicSliceSize());
		}
	}

	// update the skybox
	_deviceResources->skyBoxPass.update(swapchainIndex, glm::inverse(_projMtx * viewMtx), cameraPos, exposure);

	// submit the commandbuffer
	pvrvk::SubmitInfo submitInfo;
	pvrvk::PipelineStageFlags waitStage = pvrvk::PipelineStageFlags::e_COLOR_ATTACHMENT_OUTPUT_BIT;
	submitInfo.commandBuffers = &_deviceResources->commandBuffers[swapchainIndex];
	submitInfo.numCommandBuffers = 1;
	submitInfo.waitDstStageMask = &waitStage;
	submitInfo.waitSemaphores = &_deviceResources->imageAcquiredSemaphores[_frameId]; // wait for the acquire to be finished.
	submitInfo.numWaitSemaphores = 1;
	submitInfo.signalSemaphores = &_deviceResources->presentationSemaphores[_frameId]; // signal the semaphore when it is finish with rendering to the swapchain.
	submitInfo.numSignalSemaphores = 1;

	// submit
	_deviceResources->queue->submit(&submitInfo, 1, _deviceResources->perFrameResourcesFences[swapchainIndex]);

	if (this->shouldTakeScreenshot())
	{
		pvr::utils::takeScreenshot(_deviceResources->queue, _deviceResources->commandPool, _deviceResources->swapchain, swapchainIndex, this->getScreenshotFileName(),
			&_deviceResources->vmaAllocator, &_deviceResources->vmaAllocator);
	}

	// present
	pvrvk::PresentInfo presentInfo;
	presentInfo.waitSemaphores = &_deviceResources->presentationSemaphores[_frameId];
	presentInfo.numWaitSemaphores = 1;
	presentInfo.numSwapchains = 1;
	presentInfo.swapchains = &_deviceResources->swapchain;
	presentInfo.imageIndices = &swapchainIndex;
	_deviceResources->queue->present(presentInfo);

	_frameId = (_frameId + 1) % _deviceResources->swapchain->getSwapchainLength();

	return pvr::Result::Success;
}

/*!*********************************************************************************************************************
\brief  Pre-record the rendering commands
***********************************************************************************************************************/
void VulkanImageBasedLighting::recordCommandBuffers(uint32_t swapIndex)
{
	const pvrvk::ClearValue clearValues[] = { pvrvk::ClearValue(0.0f, 0.0f, 0.0f, 1.0f), pvrvk::ClearValue(1.f, 0) };

	// begin recording commands
	_deviceResources->commandBuffers[swapIndex]->begin();

	// begin the renderpass
	_deviceResources->commandBuffers[swapIndex]->beginRenderPass(
		_deviceResources->onScreenFramebuffer[swapIndex], pvrvk::Rect2D(0, 0, getWidth(), getHeight()), true, clearValues, ARRAY_SIZE(clearValues));

	// Render the sky box
	_deviceResources->skyBoxPass.recordCommands(_deviceResources->commandBuffers[swapIndex], swapIndex);

	uint32_t offsets[1];
	// get the matrix array offset
	offsets[0] = _deviceResources->uboPerFrame.view.getDynamicSliceOffset(swapIndex);

	// bind the descriptor sets
	_deviceResources->commandBuffers[swapIndex]->bindDescriptorSets(
		pvrvk::PipelineBindPoint::e_GRAPHICS, _deviceResources->pipelineLayout, 0, _deviceResources->descSets, ARRAY_SIZE(_deviceResources->descSets), offsets, 1);

	if (_currentModel == Models::Helmet)
	{
		_deviceResources->helmetPass.recordCommands(_deviceResources->commandBuffers[swapIndex]);
	}
	else
	{
		_deviceResources->spherePass.recordCommands(_deviceResources->commandBuffers[swapIndex]);
	}

	// record the ui renderer.
	_deviceResources->uiRenderer.beginRendering(_deviceResources->commandBuffers[swapIndex]);
	_deviceResources->uiRenderer.getDefaultTitle()->render();
	_deviceResources->uiRenderer.getDefaultControls()->render();
	_deviceResources->uiRenderer.getSdkLogo()->render();
	_deviceResources->uiRenderer.endRendering();

	_deviceResources->commandBuffers[swapIndex]->endRenderPass();
	_deviceResources->commandBuffers[swapIndex]->end();
}

void VulkanImageBasedLighting::createDescriptorSetLayouts()
{
	// Create the descriptor set layouts

	// Dynamic UBO: Transformation matrix etc.
	{
		pvrvk::DescriptorSetLayoutCreateInfo descSetInfo;
		descSetInfo.setBinding(0, pvrvk::DescriptorType::e_UNIFORM_BUFFER_DYNAMIC, 1, pvrvk::ShaderStageFlags::e_VERTEX_BIT | pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 0
		descSetInfo.setBinding(1, pvrvk::DescriptorType::e_UNIFORM_BUFFER, 1, pvrvk::ShaderStageFlags::e_VERTEX_BIT); // binding 1
		_deviceResources->descSetLayouts[DescSetIndex::PerFrame] = _deviceResources->device->createDescriptorSetLayout(descSetInfo);
	}

	// "Static" UBO: Scene maps (environment, irradiance)
	{
		pvrvk::DescriptorSetLayoutCreateInfo descSetInfo;
		descSetInfo.setBinding(0, pvrvk::DescriptorType::e_UNIFORM_BUFFER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 0
		descSetInfo.setBinding(1, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 2: Diffuse irradianceMap
		descSetInfo.setBinding(2, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 3: Specular irradianceMap
		descSetInfo.setBinding(3, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 4: Environment map (for perfect reflections)
		descSetInfo.setBinding(4, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 5: brdfLUTmap
		_deviceResources->descSetLayouts[DescSetIndex::Model] = _deviceResources->device->createDescriptorSetLayout(descSetInfo);
	}

	// Material textures textures
	{
		pvrvk::DescriptorSetLayoutCreateInfo descSetInfo;
		descSetInfo.setBinding(0, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 0: Albedo
		descSetInfo.setBinding(1, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 1: MetallicRoughness
		descSetInfo.setBinding(2, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 2: Normal
		descSetInfo.setBinding(3, pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 3: Emissive
		descSetInfo.setBinding(4, pvrvk::DescriptorType::e_UNIFORM_BUFFER, 1, pvrvk::ShaderStageFlags::e_FRAGMENT_BIT); // binding 1
		_deviceResources->descSetLayouts[DescSetIndex::Material] = _deviceResources->device->createDescriptorSetLayout(descSetInfo);
	}
}

void VulkanImageBasedLighting::createPipelineLayout()
{
	// create the pipeline layout
	pvrvk::PipelineLayoutCreateInfo pipeLayoutInfo;
	pipeLayoutInfo.addDescSetLayout(_deviceResources->descSetLayouts[0]);
	pipeLayoutInfo.addDescSetLayout(_deviceResources->descSetLayouts[1]);
	pipeLayoutInfo.addDescSetLayout(_deviceResources->descSetLayouts[2]);

	pipeLayoutInfo.setPushConstantRange(0,
		pvrvk::PushConstantRange(
			pvrvk::ShaderStageFlags::e_VERTEX_BIT | pvrvk::ShaderStageFlags::e_FRAGMENT_BIT, 0, static_cast<uint32_t>(pvr::getSize(pvr::GpuDatatypes::Integer) * 2)));

	_deviceResources->pipelineLayout = _deviceResources->device->createPipelineLayout(pipeLayoutInfo);
}

/*!*********************************************************************************************************************
\brief  Creates the buffers used throughout the demo.
***********************************************************************************************************************/
void VulkanImageBasedLighting::createUbos()
{
	// Per frame
	{
		pvr::utils::StructuredMemoryDescription desc;
		desc.addElement("view", pvr::GpuDatatypes::mat4x4);
		desc.addElement("MVP", pvr::GpuDatatypes::mat4x4);
		desc.addElement("camPos", pvr::GpuDatatypes::vec3);
		desc.addElement("emissiveScale", pvr::GpuDatatypes::Float);
		desc.addElement("exposure", pvr::GpuDatatypes::Float);

		_deviceResources->uboPerFrame.view.initDynamic(desc, _deviceResources->swapchain->getSwapchainLength(), pvr::BufferUsageFlags::UniformBuffer,
			static_cast<uint32_t>(_deviceResources->device->getPhysicalDevice()->getProperties().getLimits().getMinUniformBufferOffsetAlignment()));

		const pvrvk::DeviceSize size = _deviceResources->uboPerFrame.view.getSize();
		_deviceResources->uboPerFrame.buffer =
			pvr::utils::createBuffer(_deviceResources->device, size, pvrvk::BufferUsageFlags::e_UNIFORM_BUFFER_BIT, pvrvk::MemoryPropertyFlags::e_HOST_VISIBLE_BIT,
				pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT | pvrvk::MemoryPropertyFlags::e_DEVICE_LOCAL_BIT, &_deviceResources->vmaAllocator);

		_deviceResources->uboPerFrame.view.pointToMappedMemory(_deviceResources->uboPerFrame.buffer->getDeviceMemory()->getMappedData());
	}

	// World matrices (Helmet and spheres)
	{
		pvr::utils::StructuredMemoryDescription desc;
		desc.addElement("model", pvr::GpuDatatypes::mat4x4, NumInstances + 1);
		desc.addElement("modelInvTranspose", pvr::GpuDatatypes::mat3x3, NumInstances + 1);

		_deviceResources->uboWorld.view.init(desc);

		const pvrvk::DeviceSize size = _deviceResources->uboWorld.view.getSize();
		_deviceResources->uboWorld.buffer =
			pvr::utils::createBuffer(_deviceResources->device, size, pvrvk::BufferUsageFlags::e_UNIFORM_BUFFER_BIT, pvrvk::MemoryPropertyFlags::e_HOST_VISIBLE_BIT,
				pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT | pvrvk::MemoryPropertyFlags::e_DEVICE_LOCAL_BIT, &_deviceResources->vmaAllocator);
		_deviceResources->uboWorld.view.pointToMappedMemory(_deviceResources->uboWorld.buffer->getDeviceMemory()->getMappedData());
		// set the helmet matrix

		glm::mat4 model = glm::eulerAngleXY(glm::radians(0.f), glm::radians(120.f)) * glm::scale(glm::vec3(22.0f));
		_deviceResources->uboWorld.view.getElement(0).setValue(model);
		_deviceResources->uboWorld.view.getElement(1).setValue(glm::inverseTranspose(model));

		// set the sphere matrices
		float positionOffsetX = -25.f;
		float positionOffsetY = 15.f;

		for (uint32_t i = 0; i < NumInstances; ++i)
		{
			if ((i % NumSphereColumns) == 0)
			{
				positionOffsetX = -25.0f;
			}

			if ((i != 0) && (i % NumSphereColumns == 0))
			{
				positionOffsetY -= 10.f;
			}

			const glm::mat4 model = glm::translate(glm::vec3(positionOffsetX, positionOffsetY, 0.0f)) * glm::scale(glm::vec3(4.5f));
			positionOffsetX += 10.f;
			_deviceResources->uboWorld.view.getElement(0, i + 1).setValue(model);
			_deviceResources->uboWorld.view.getElement(1, i + 1).setValue(glm::inverseTranspose(model));
		}

		if ((_deviceResources->uboWorld.buffer->getDeviceMemory()->getMemoryFlags() & pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT) == 0)
		{
			_deviceResources->uboWorld.buffer->getDeviceMemory()->flushRange();
		}
	}

	// Ubo lights
	{
		pvr::utils::StructuredMemoryDescription desc;
		desc.addElement("lights", pvr::GpuDatatypes::vec3);
		desc.addElement("numSpecularIrrMapMipLevels", pvr::GpuDatatypes::uinteger);

		_deviceResources->uboLights.view.init(desc);
		const pvrvk::DeviceSize size = _deviceResources->uboLights.view.getSize();
		_deviceResources->uboLights.buffer =
			pvr::utils::createBuffer(_deviceResources->device, size, pvrvk::BufferUsageFlags::e_UNIFORM_BUFFER_BIT, pvrvk::MemoryPropertyFlags::e_HOST_VISIBLE_BIT,
				pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT | pvrvk::MemoryPropertyFlags::e_DEVICE_LOCAL_BIT, &_deviceResources->vmaAllocator);

		_deviceResources->uboLights.view.pointToMappedMemory(_deviceResources->uboLights.buffer->getDeviceMemory()->getMappedData());

		_deviceResources->uboLights.view.getElement(0).setValue(lightDir);
		_deviceResources->uboLights.view.getElement(1).setValue(_deviceResources->skyBoxPass.getNumPrefilteredMipLevels());

		if (uint32_t(_deviceResources->uboLights.buffer->getDeviceMemory()->getMemoryFlags() & pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT) == 0)
		{
			_deviceResources->uboLights.buffer->getDeviceMemory()->flushRange();
		}
	}

	// ubo material
	{
		const pvr::utils::StructuredMemoryDescription materialDesc(
			"material", NumInstances + 1, { { "roughness", pvr::GpuDatatypes::Float }, { "metallic", pvr::GpuDatatypes::Float }, { "rgb", pvr::GpuDatatypes::vec3 } });

		_deviceResources->uboMaterial.view.init(pvr::utils::StructuredMemoryDescription("materials", 1, { materialDesc }));

		_deviceResources->uboMaterial.buffer = pvr::utils::createBuffer(_deviceResources->device, _deviceResources->uboMaterial.view.getSize(),
			pvrvk::BufferUsageFlags::e_UNIFORM_BUFFER_BIT, pvrvk::MemoryPropertyFlags::e_HOST_VISIBLE_BIT,
			pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT | pvrvk::MemoryPropertyFlags::e_DEVICE_LOCAL_BIT, &_deviceResources->vmaAllocator);

		_deviceResources->uboMaterial.view.pointToMappedMemory(_deviceResources->uboMaterial.buffer->getDeviceMemory()->getMappedData());

		// update the material buffer
		pvr::assets::Material& material = _deviceResources->helmetPass.getModel()->getMaterial(0);
		pvr::assets::Material::GLTFMetallicRoughnessSemantics metallicRoughness(material);

		// Helmet material
		auto helmetView = _deviceResources->uboMaterial.view.getElement(0, 0);
		helmetView.getElement(0).setValue(metallicRoughness.getRoughness());
		helmetView.getElement(1).setValue(metallicRoughness.getMetallicity());
		helmetView.getElement(2).setValue(metallicRoughness.getBaseColor());

		// Spheres materials

		// offset the posittion for each sphere instances
		const glm::vec3 color[] = {
			glm::vec3(0.971519, 0.959915, 0.915324), // Silver Metallic
			glm::vec3(1, 0.765557, 0.336057), // Gold Metallic
			glm::vec3(.75f), // White Plastic
			glm::vec3(.01f, .05f, .2f), // Blue Plastic
		};

		const float roughness[NumSphereColumns] = { .9f, 0.6f, 0.35f, 0.25f, 0.15f, 0.0f };

		// set the per sphere materiual property.
		for (uint32_t i = 0; i < NumSphereRows; ++i)
		{
			for (uint32_t j = 0; j < NumSphereColumns; ++j)
			{
				auto sphereView = _deviceResources->uboMaterial.view.getElement(0, i * NumSphereColumns + j + 1);
				sphereView.getElement(0).setValue(roughness[j]);
				sphereView.getElement(1).setValue(float(i < 2) * 1.0f); // set the first 2 row metalicity and the remaining to 0.0
				sphereView.getElement(2).setValue(color[i]);
			}
		}

		if ((_deviceResources->uboMaterial.buffer->getDeviceMemory()->getMemoryFlags() & pvrvk::MemoryPropertyFlags::e_HOST_COHERENT_BIT) == 0)
		{
			_deviceResources->uboMaterial.buffer->getDeviceMemory()->flushRange();
		}
	}
}

/*!*********************************************************************************************************************
\brief  Create combined texture and sampler descriptor set for the materials in the _scene
\return Return true on success
***********************************************************************************************************************/
void VulkanImageBasedLighting::updateDescriptors()
{
	// Update the descriptor sets

	std::vector<pvrvk::WriteDescriptorSet> writeDescSets;
	// Dynamic ubo (per frame/object data) : Transformation matrices
	{
		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_UNIFORM_BUFFER_DYNAMIC, _deviceResources->descSets[0], 0));
		writeDescSets.back().setBufferInfo(0, pvrvk::DescriptorBufferInfo(_deviceResources->uboPerFrame.buffer, 0, _deviceResources->uboPerFrame.view.getDynamicSliceSize()));

		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_UNIFORM_BUFFER, _deviceResources->descSets[0], 1));
		writeDescSets.back().setBufferInfo(0, pvrvk::DescriptorBufferInfo(_deviceResources->uboWorld.buffer, 0, _deviceResources->uboWorld.view.getSize()));
	}

	// Static ubo (per scene data) : Environment maps etc., BRDF
	{
		// Light
		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_UNIFORM_BUFFER, _deviceResources->descSets[1], 0));
		writeDescSets.back().setBufferInfo(0, pvrvk::DescriptorBufferInfo(_deviceResources->uboLights.buffer, 0, _deviceResources->uboLights.view.getDynamicSliceSize()));

		// Diffuse Irradiance
		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, _deviceResources->descSets[1], 1));
		writeDescSets.back().setImageInfo(0,
			pvrvk::DescriptorImageInfo(_deviceResources->skyBoxPass.getDiffuseIrradianceMap(), _deviceResources->samplerTrilinear, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));

		// Specular Irradiance
		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, _deviceResources->descSets[1], 2));
		writeDescSets.back().setImageInfo(
			0, pvrvk::DescriptorImageInfo(_deviceResources->skyBoxPass.getPrefilteredMap(), _deviceResources->samplerTrilinear, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));

		// Environment map
		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, _deviceResources->descSets[1], 3));
		writeDescSets.back().setImageInfo(0,
			pvrvk::DescriptorImageInfo(
				_deviceResources->skyBoxPass.getPrefilteredMipMap(), _deviceResources->samplerTrilinearLodClamped, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));

		// BRDF LUT
		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, _deviceResources->descSets[1], 4));
		writeDescSets.back().setImageInfo(0, pvrvk::DescriptorImageInfo(_deviceResources->brdfLUT, _deviceResources->samplerBilinear, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));
	}
	// Per object ubo: Material textures.
	{
		// Albedo Map
		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, _deviceResources->descSets[2], 0));
		writeDescSets.back().setImageInfo(
			0, pvrvk::DescriptorImageInfo(_deviceResources->helmetPass.getAlbedoMap(), _deviceResources->samplerBilinear, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));

		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, _deviceResources->descSets[2], 1));
		writeDescSets.back().setImageInfo(0,
			pvrvk::DescriptorImageInfo(
				_deviceResources->helmetPass.getOcclusionMetallicRoughnessMap(), _deviceResources->samplerBilinear, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));

		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, _deviceResources->descSets[2], 2));
		writeDescSets.back().setImageInfo(
			0, pvrvk::DescriptorImageInfo(_deviceResources->helmetPass.getNormalMap(), _deviceResources->samplerBilinear, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));

		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_COMBINED_IMAGE_SAMPLER, _deviceResources->descSets[2], 3));
		writeDescSets.back().setImageInfo(
			0, pvrvk::DescriptorImageInfo(_deviceResources->helmetPass.getEmissiveMap(), _deviceResources->samplerBilinear, pvrvk::ImageLayout::e_SHADER_READ_ONLY_OPTIMAL));

		// Materials buffers
		writeDescSets.push_back(pvrvk::WriteDescriptorSet(pvrvk::DescriptorType::e_UNIFORM_BUFFER, _deviceResources->descSets[2], 4));
		writeDescSets.back().setBufferInfo(0, pvrvk::DescriptorBufferInfo(_deviceResources->uboMaterial.buffer, 0, _deviceResources->uboMaterial.view.getDynamicSliceSize()));
	}

	_deviceResources->device->updateDescriptorSets(writeDescSets.data(), static_cast<uint32_t>(writeDescSets.size()), nullptr, 0);
}

/// <summary>This function must be implemented by the user of the shell. The user should return its pvr::Shell object defining the behaviour of the application.</summary>
/// <returns>Return a unique ptr to the demo supplied by the user.</returns>
std::unique_ptr<pvr::Shell> pvr::newDemo()
{
	return std::unique_ptr<pvr::Shell>(new VulkanImageBasedLighting());
}
