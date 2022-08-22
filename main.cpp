/*
 * Copyright (c) 2014-2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2014-2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */


// ImGui - standalone example application for Glfw + Vulkan, using programmable
// pipeline If you are new to ImGui, see examples/README.txt and documentation
// at the top of imgui.cpp.

#include <iostream>
#include <array>
#include <unordered_map>
#include <dxgi1_4.h>
#include <atlbase.h>

#include "backends/imgui_impl_glfw.h"
#include "imgui.h"

#include "hello_vulkan.h"
#include "imgui/imgui_camera_widget.h"
#include "nvh/cameramanipulator.hpp"
#include "nvh/fileoperations.hpp"
#include "nvpsystem.hpp"
#include "nvvk/commands_vk.hpp"
#include "nvvk/context_vk.hpp"


//////////////////////////////////////////////////////////////////////////
#define UNUSED(x) (void)(x)
//////////////////////////////////////////////////////////////////////////

// Default search path for shaders
std::vector<std::string> defaultSearchPaths;


// GLFW Callback functions
static void onErrorCallback(int error, const char* description)
{
  fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// Extra UI
void renderUI(HelloVulkan& helloVk)
{
  ImGuiH::CameraWidget();
  if(ImGui::CollapsingHeader("Light"))
  {
    ImGui::RadioButton("Point", &helloVk.m_pcRaster.lightType, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Infinite", &helloVk.m_pcRaster.lightType, 1);

    ImGui::SliderFloat3("Position", &helloVk.m_pcRaster.lightPosition.x, -20.f, 20.f);
    ImGui::SliderFloat("Intensity", &helloVk.m_pcRaster.lightIntensity, 0.f, 150.f);
  }
}

struct FramePerformanceCounter {
    int frameCount = 0;
    int maxFrames;
    std::vector<std::chrono::microseconds> frameTimes;
    std::chrono::steady_clock::time_point begin, end;
    std::string filename;
    void init(int maxFrames, std::string filename)
    {
      frameTimes.reserve(maxFrames);
      this->maxFrames = maxFrames;
      this->filename = filename;
    }

    void startFrameTiming()
    {
      begin = std::chrono::steady_clock::now();
    }

    void endFrameTiming()
    {
      end = std::chrono::steady_clock::now();
      std::chrono::microseconds currentFrameTime = std::chrono::duration_cast<std::chrono::microseconds>(end - begin);
      frameTimes.emplace_back(currentFrameTime);
      ++frameCount;

      if (frameCount == maxFrames)
      {
        std::ofstream outputFile(filename);
        uint32_t averageFrameTime = 0;
        for (const auto& ft : frameTimes)
        {
          averageFrameTime += ft.count();
          outputFile << ft.count()  << '\n';
        }
        averageFrameTime /= frameTimes.size();
        outputFile << averageFrameTime << '\n';
        outputFile.close();
        exit(0);
      }
    }

  };

void writeMemoryUsage(const std::string& filename) 
{
  IDXGIFactory4* dxgiFactory = nullptr;
  CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
  IDXGIAdapter3* dxgiAdapter = nullptr;
  IDXGIAdapter1* tmpDxgiAdapter = nullptr;
  UINT adapterIndex = 0;
  while(dxgiFactory->EnumAdapters1(adapterIndex, &tmpDxgiAdapter) != DXGI_ERROR_NOT_FOUND)
  {
    DXGI_ADAPTER_DESC1 desc;
    tmpDxgiAdapter->GetDesc1(&desc);
    if(!dxgiAdapter && desc.Flags == 0)
    {
        tmpDxgiAdapter->QueryInterface(IID_PPV_ARGS(&dxgiAdapter));
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        dxgiAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
        std::ofstream outputFile(filename);
        outputFile << info.CurrentUsage / (1024 * 1024) << '\n';
        outputFile.close();
    }
    tmpDxgiAdapter->Release();
    ++adapterIndex;
  }
  dxgiFactory->Release();
}
void writeAccelBuildTime(std::string filename, int64_t length)
{
  std::ofstream outFile;
  outFile.open(filename, std::ios_base::app); // Append instead of overwrite
  outFile << length;
  outFile << '\n';
  outFile.close();
}


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////


//--------------------------------------------------------------------------------------------------
// Application Entry
//
int main(int argc, char** argv)
{
  std::string modelName = "triangle";
  int SAMPLE_WIDTH  = 1920;
  int SAMPLE_HEIGHT = 1080;
  if(argc >= 4)
  {
    SAMPLE_WIDTH = atoi(argv[1]);
    SAMPLE_HEIGHT = atoi(argv[2]);
    modelName    = std::string(argv[3]);
  }
  FramePerformanceCounter counter;
  counter.init(1000, "vulkan-"+std::to_string(SAMPLE_WIDTH)+"x"+std::to_string(SAMPLE_HEIGHT)+modelName+"-textures-frametimes.txt");
  // Setup GLFW window
  glfwSetErrorCallback(onErrorCallback);
  if(!glfwInit())
  {
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window = glfwCreateWindow(SAMPLE_WIDTH, SAMPLE_HEIGHT, PROJECT_NAME, nullptr, nullptr);


  // Setup camera
  CameraManip.setWindowSize(SAMPLE_WIDTH, SAMPLE_HEIGHT);
  CameraManip.setLookat(nvmath::vec3f(5, 4, -4), nvmath::vec3f(0, 1, 0), nvmath::vec3f(0, 1, 0));
  std::unordered_map<std::string, nvmath::vec3f> cameraCenterOffsets = {
    {"bmw", {0,0,800}}, 
    {"hairball", {10,10,10}},
    {"triangle", {10,10,10}},
    {"sponza", {1160.0f,207.0f,-65.0f}},
    {"viking_room", {0.20888,2.85559,0.43343}}
  };
  auto                                           cameraCenterOffset  = cameraCenterOffsets.at(modelName);
  auto                                           camera              = CameraManip.getCamera();
  camera.eye = cameraCenterOffset;
  if(modelName == "sponza")
    camera.ctr = {-22.0f, 252.0f, -58.2f};
  
  CameraManip.setCamera(camera, true);
  // Setup Vulkan
  if(!glfwVulkanSupported())
  {
    printf("GLFW: Vulkan Not Supported\n");
    return 1;
  }

  // setup some basic things for the sample, logging file for example
  NVPSystem system(PROJECT_NAME);

  // Search path for shaders and other media
  defaultSearchPaths = {
      NVPSystem::exePath() + PROJECT_RELDIRECTORY,
      NVPSystem::exePath() + PROJECT_RELDIRECTORY "..",
      std::string(PROJECT_NAME),
  };

  // Vulkan required extensions
  assert(glfwVulkanSupported() == 1);
  uint32_t count{0};
  auto     reqExtensions = glfwGetRequiredInstanceExtensions(&count);

  // Requesting Vulkan extensions and layers
  nvvk::ContextCreateInfo contextInfo;
  contextInfo.setVersion(1, 2);                       // Using Vulkan 1.2
  for(uint32_t ext_id = 0; ext_id < count; ext_id++)  // Adding required extensions (surface, win32, linux, ..)
    contextInfo.addInstanceExtension(reqExtensions[ext_id]);
  contextInfo.addInstanceLayer("VK_LAYER_LUNARG_monitor", true);              // FPS in titlebar
  contextInfo.addInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, true);  // Allow debug names
  contextInfo.addDeviceExtension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);            // Enabling ability to present rendering

  // #VKRay: Activate the ray tracing extension
  VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  contextInfo.addDeviceExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, false, &accelFeature);  // To build acceleration structures
  VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
  contextInfo.addDeviceExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, false, &rtPipelineFeature);  // To use vkCmdTraceRaysKHR
  contextInfo.addDeviceExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);  // Required by ray tracing pipeline

  // Creating Vulkan base application
  nvvk::Context vkctx{};
  vkctx.initInstance(contextInfo);
  // Find all compatible devices
  auto compatibleDevices = vkctx.getCompatibleDevices(contextInfo);
  assert(!compatibleDevices.empty());
  // Use a compatible device
  vkctx.initDevice(compatibleDevices[0], contextInfo);

  // Create example
  HelloVulkan helloVk;

  // Window need to be opened to get the surface on which to draw
  const VkSurfaceKHR surface = helloVk.getVkSurface(vkctx.m_instance, window);
  vkctx.setGCTQueueWithPresent(surface);

  helloVk.setup(vkctx.m_instance, vkctx.m_device, vkctx.m_physicalDevice, vkctx.m_queueGCT.familyIndex);
  helloVk.createSwapchain(surface, SAMPLE_WIDTH, SAMPLE_HEIGHT);
  helloVk.createDepthBuffer();
  helloVk.createRenderPass();
  helloVk.createFrameBuffers();

  // Setup Imgui
  helloVk.initGUI(0);  // Using sub-pass 0

  // Creation of the example
  //helloVk.loadModel(nvh::findFile("media/models/sponza.obj", defaultSearchPaths, true));
  helloVk.loadModel(nvh::findFile("media/models/"+modelName+"/"+modelName+".obj", defaultSearchPaths, true));
  //helloVk.loadModel(nvh::findFile("media/models/isGardeniaA/isGardeniaA.obj", defaultSearchPaths, true));
  //helloVk.loadModel(nvh::findFile("media/models/isBeach/isBeach.obj", defaultSearchPaths, true));
  //helloVk.loadModel(nvh::findFile("media/scenes/plane.obj", defaultSearchPaths, true));

  helloVk.createOffscreenRender();
  helloVk.createDescriptorSetLayout();
  helloVk.createGraphicsPipeline();
  helloVk.createUniformBuffer();
  helloVk.createObjDescriptionBuffer();
  helloVk.updateDescriptorSet();

  helloVk.m_pcRaster.lightType = 1;
  helloVk.m_pcRaster.lightIntensity = 1.0f;
  helloVk.m_pcRay.lightType    = 1;
  // #VKRay
  std::string filenamePrefix =
      "vulkan-" + std::to_string(SAMPLE_WIDTH) + "x" + std::to_string(SAMPLE_HEIGHT) + modelName + "-textures";
  helloVk.setFilenamePrefix(filenamePrefix);
  helloVk.initRayTracing();
  auto start = std::chrono::high_resolution_clock::now();
  helloVk.createBottomLevelAS();
  helloVk.createTopLevelAS();
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::microseconds length = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  writeAccelBuildTime(filenamePrefix + "-accelbuildtimes.txt", length.count());
  exit(0);
  
  helloVk.createRtDescriptorSet();
  helloVk.createRtPipeline();
  helloVk.createRtShaderBindingTable();

  helloVk.createPostDescriptor();
  helloVk.createPostPipeline();
  helloVk.updatePostDescriptorSet();


  nvmath::vec4f clearColor   = nvmath::vec4f(1, 1, 1, 1.00f);
  bool          useRaytracer = true;


  helloVk.setupGlfwCallbacks(window);
  ImGui_ImplGlfw_InitForVulkan(window, true);

  writeMemoryUsage("vulkan-" + std::to_string(SAMPLE_WIDTH) + "x" + std::to_string(SAMPLE_HEIGHT) + modelName
                   + "-textures-memoryusage.txt");
  // Main loop
  while(!glfwWindowShouldClose(window))
  {
    glfwPollEvents();
    if(helloVk.isMinimized())
      continue;

    // Start the Dear ImGui frame
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();


    // Show UI window.
    if(helloVk.showGui())
    {
      ImGuiH::Panel::Begin();
      ImGui::ColorEdit3("Clear color", reinterpret_cast<float*>(&clearColor));
      ImGui::Checkbox("Ray Tracer mode", &useRaytracer);  // Switch between raster and ray tracing

      renderUI(helloVk);
      ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
      ImGuiH::Control::Info("", "", "(F10) Toggle Pane", ImGuiH::Control::Flags::Disabled);
      ImGuiH::Panel::End();
    }

    // Start rendering the scene
    counter.startFrameTiming();
    helloVk.prepareFrame();

    // Start command buffer of this frame
    auto                   curFrame = helloVk.getCurFrame();
    const VkCommandBuffer& cmdBuf   = helloVk.getCommandBuffers()[curFrame];

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmdBuf, &beginInfo);

    // Updating camera buffer
    helloVk.updateUniformBuffer(cmdBuf);

    // Clearing screen
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color        = {{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}};
    clearValues[1].depthStencil = {1.0f, 0};

    // Offscreen render pass
    {
      VkRenderPassBeginInfo offscreenRenderPassBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      offscreenRenderPassBeginInfo.clearValueCount = 2;
      offscreenRenderPassBeginInfo.pClearValues    = clearValues.data();
      offscreenRenderPassBeginInfo.renderPass      = helloVk.m_offscreenRenderPass;
      offscreenRenderPassBeginInfo.framebuffer     = helloVk.m_offscreenFramebuffer;
      offscreenRenderPassBeginInfo.renderArea      = {{0, 0}, helloVk.getSize()};

      // Rendering Scene
      if(useRaytracer)
      {
        helloVk.raytrace(cmdBuf, clearColor);
      }
      else
      {
        vkCmdBeginRenderPass(cmdBuf, &offscreenRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        helloVk.rasterize(cmdBuf);
        vkCmdEndRenderPass(cmdBuf);
      }
    }

    // 2nd rendering pass: tone mapper, UI
    {
      VkRenderPassBeginInfo postRenderPassBeginInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      postRenderPassBeginInfo.clearValueCount = 2;
      postRenderPassBeginInfo.pClearValues    = clearValues.data();
      postRenderPassBeginInfo.renderPass      = helloVk.getRenderPass();
      postRenderPassBeginInfo.framebuffer     = helloVk.getFramebuffers()[curFrame];
      postRenderPassBeginInfo.renderArea      = {{0, 0}, helloVk.getSize()};

      // Rendering tonemapper
      vkCmdBeginRenderPass(cmdBuf, &postRenderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
      helloVk.drawPost(cmdBuf);
      // Rendering UI
      ImGui::Render();
      ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdBuf);
      vkCmdEndRenderPass(cmdBuf);
      counter.endFrameTiming();
    }

    // Submit for display
    vkEndCommandBuffer(cmdBuf);
    helloVk.submitFrame();
  }

  // Cleanup
  vkDeviceWaitIdle(helloVk.getDevice());

  helloVk.destroyResources();
  helloVk.destroy();
  vkctx.deinit();

  glfwDestroyWindow(window);
  glfwTerminate();

  return 0;
}
