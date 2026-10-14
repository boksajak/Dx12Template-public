#include "common.h"
#include "scene.h"
#include "utils.h"
#include "D3D12Helpers.h"
#include "GPUProfiler.h"
#include "TAA.h"
#include "RTPass.h"
#include "ComputePass.h"
#include "D3D12ResourceUploader.h"
#include "DxcShaderCompiler.h"
#include "shaders/shared.h"

#include <cstdlib>
#include <filesystem>

// Windows DPI Scaling
#include <ShellScalingApi.h>
#pragma comment(lib, "shcore.lib")

using namespace d3d12Helpers;

class Dx12Template
{
public:
	friend void readCmdLineArguments(Dx12Template* dx12Template, LPWSTR lpCmdLine);

	bool Initialize(HWND hwnd)
	{
		mDpiScale = utils::getDpiScale(hwnd);
		mShaderData.frameNumber = 0;
		mShaderData.triangleLightCount = 0;
		mReloadShaders = false;

		mShaderCompiler.Initialize();

		// Load scene and make a list of scenes available
		loadScene();
		makeSceneList();

		// Initialize DirectX 12 and ImGui
		{
			initializeDx12(hwnd);
			updateRendererMemory();

			for (auto& profiler : mProfiler) profiler.Initialize(mDevice);

			finishSceneUpload();
		}
		return initImGui(hwnd);
	}

	bool ReloadScene(std::wstring sceneFile, std::wstring cameraFile = L"")
	{
		// Wait until pending resources are finished
		finishSceneUpload();
		releaseSceneResources();

		// Remember new setting
		mSceneFile = sceneFile;
		mCameraFile = cameraFile;

		// Load new scene
		bool sceneLoaded = loadScene();

		// Upload scene and update views
		createSceneResources();
		createResourceViews();

		// Wait until scene upload is finished
		finishSceneUpload();

		return sceneLoaded;
	}

	bool ProcessSceneChange()
	{
		if (!mLoadSceneRequested) return false;
		mLoadSceneRequested = false;
		ReloadScene(mSceneFiles[mSelectedScene], mSceneFiles[mSelectedScene] == mSceneFile ? mCameraFile : L"");
		return true;
	}

	void ReloadShaders()
	{
		mReloadShaders = true;
	}

	void ToggleGUI()
	{
		mShowGUI = !mShowGUI;
	}

	void SetFrameSize(int width, int height)
	{
		if (width != mFrameWidth || height != mFrameHeight) resetAccumulation();
		mFrameWidth = width;
		mFrameHeight = height;
	}

	void GetFrameSize(int& width, int& height)
	{
		width = mFrameWidth;
		height = mFrameHeight;
	}

	void SetSceneFiles(std::wstring sceneFile, std::wstring cameraFile)
	{
		mSceneFile = sceneFile;
		mCameraFile = cameraFile;
	}

	void HandleCameraMovement(HWND hWnd, float elapsedTime)
	{
		ImGuiIO& io = ImGui::GetIO();

		// Discard mouse history while the window is inactive to avoid a jump when it resumes.
		if (GetForegroundWindow() != hWnd)
		{
			mLastMousePosition = glm::vec2(FLT_MAX);
			return;
		}

		float yaw = 0.0f;
		float pitch = 0.0f;

		// Read the keyboard state to see if any movement keys have been pressed
		BYTE keyboardState[256] = { 0 };
		if (!io.WantCaptureKeyboard && GetKeyboardState(keyboardState)) {
		
			// Handle camera translation
			{
				float3 movementDir = float3(0.0f, 0.0f, 0.0f);

				if (utils::isKeyPressed(keyboardState, 'W')) movementDir += mCameraForward;
				if (utils::isKeyPressed(keyboardState, 'S')) movementDir -= mCameraForward;
				if (utils::isKeyPressed(keyboardState, 'D')) movementDir += mCameraRight;
				if (utils::isKeyPressed(keyboardState, 'A')) movementDir -= mCameraRight;
				if (utils::isKeyPressed(keyboardState, 33)) movementDir += mCameraUp;
				if (utils::isKeyPressed(keyboardState, 'E')) movementDir += mCameraUp;
				if (utils::isKeyPressed(keyboardState, 34)) movementDir -= mCameraUp;
				if (utils::isKeyPressed(keyboardState, 'Q')) movementDir -= mCameraUp;

				// Move the camera
				if (glm::length(movementDir) > 0.0f)
				{
					// Figure out the movement speed
					float movementSpeed = mCameraMovementSpeed * elapsedTime;
					if (utils::isKeyPressed(keyboardState, VK_LSHIFT) || utils::isKeyPressed(keyboardState, VK_RSHIFT))
					{
						movementSpeed *= 2.0f;
					}
					if (utils::isKeyPressed(keyboardState, VK_LCONTROL) || utils::isKeyPressed(keyboardState, VK_RCONTROL))
					{
						movementSpeed *= 0.2f;
					}
				
					mCameraPosition += glm::normalize(movementDir) * movementSpeed;
				}
			}

			// Handle camera rotation
			{
				if (utils::isKeyPressed(keyboardState, VK_NUMPAD8)) pitch += 1.0f;
				if (utils::isKeyPressed(keyboardState, VK_UP)) pitch += 1.0f;
				if (utils::isKeyPressed(keyboardState, VK_NUMPAD2)) pitch -= 1.0f;
				if (utils::isKeyPressed(keyboardState, VK_DOWN)) pitch -= 1.0f;
				if (utils::isKeyPressed(keyboardState, VK_NUMPAD6)) yaw += 1.0f;
				if (utils::isKeyPressed(keyboardState, VK_RIGHT)) yaw += 1.0f;
				if (utils::isKeyPressed(keyboardState, VK_NUMPAD4)) yaw -= 1.0f;
				if (utils::isKeyPressed(keyboardState, VK_LEFT)) yaw -= 1.0f;

				yaw *= mCameraRotationSpeed * elapsedTime;
				pitch *= mCameraRotationSpeed * elapsedTime;
			}
		}

		// Handle rotation by mouse
		if (!io.WantCaptureMouse)
		{
			if (GetKeyState(VK_LBUTTON) & 0x8000)
			{
				if (mLastMousePosition.x != FLT_MAX)
				{
					const glm::vec2 mouseMovement = utils::getMousePosition(hWnd, mFrameWidth, mFrameHeight) - mLastMousePosition;

					yaw += mCameraMouseRotationSpeed * elapsedTime * mouseMovement.x;
					pitch -= mCameraMouseRotationSpeed * elapsedTime * mouseMovement.y;

				}
			}
		
			mLastMousePosition = utils::getMousePosition(hWnd, mFrameWidth, mFrameHeight);
		}
		else
		{
			mLastMousePosition = glm::vec2(FLT_MAX);
		}

		// Apply camera rotations
		{
			const glm::vec3 rotationUp = glm::vec3(0.0f, 1.0f, 0.0f);

			// Apply pitch
			if (pitch != 0.0f)
			{
				glm::mat4 pitchMatrix = glm::rotate(glm::identity<glm::mat4>(), pitch, mCameraRight);
				mCameraForward = glm::normalize(pitchMatrix * glm::vec4(mCameraForward, 0.0f));
				mCameraUp = glm::normalize(pitchMatrix * glm::vec4(mCameraUp, 0.0f));
			}

			// Apply yaw
			if (yaw != 0.0f)
			{
				glm::mat4 yawMatrix = glm::rotate(glm::identity<glm::mat4>(), yaw, rotationUp);
				mCameraForward = glm::normalize(yawMatrix * glm::vec4(mCameraForward, 0.0f));
			}

			mCameraRight = glm::normalize(glm::cross(rotationUp, mCameraForward));
			mCameraUp = glm::normalize(glm::cross(mCameraForward, mCameraRight));
		}
	}

	void drawGUI(const std::string& profiling, const float elapsedTime)
	{
		// Start the Dear ImGui frame
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// Position the ImGui window on start to the right
		if (ImGui::GetFrameCount() == 1) {
			ImGui::SetNextWindowPos(ImVec2(mFrameWidth - 450 - 20, 20));
			ImGui::SetNextWindowSize(ImVec2(450, mFrameHeight - 40.f));
		}

		ImGui::Begin("Dx12Template");

		static float elapsedTimeWeighted = 0.0f;
		static const float alpha = 0.95f;
		elapsedTimeWeighted = alpha * elapsedTimeWeighted + (1.0f - alpha) * elapsedTime;
		ImGui::Text("Frame Time: %.02fms", elapsedTimeWeighted);

		ImGui::Text("Adapter: %ls", mAdapterName.c_str());
		ImGui::Checkbox("VSync", &mEnableVSync);
		if (ImGui::Button("[F5] Reload Shaders")) mReloadShaders = true;
		ImGui::SameLine();
		ImGui::Text("[F1] Toggle GUI");
		float cameraSpeed = mCameraMovementSpeed * 1000.0f; // Display scene units per second; input uses milliseconds.
		if (ImGui::SliderFloat("Camera speed", &cameraSpeed, 0.1f, 100.0f, "%.1f units/s", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) mCameraMovementSpeed = cameraSpeed * 0.001f;
		ImGui::Separator();

		// Scene selection is applied between frames, after submitting this frame's GUI.
		if (ImGui::BeginCombo("Scene", mSceneNames[mSelectedScene].c_str()))
		{
			for (size_t i = 0; i < mSceneNames.size(); i++)
			{
				ImGui::PushID(int(i));
				if (ImGui::Selectable(mSceneNames[i].c_str(), mSelectedScene == int(i)))
				{
					mSelectedScene = int(i);
					mLoadSceneRequested = true;
				}
				if (mSelectedScene == int(i)) ImGui::SetItemDefaultFocus();
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		if (ImGui::Button("Reload scene")) mLoadSceneRequested = true;
		ImGui::Separator();

		// Renderer settings GUI
		{
			ImGui::Text("Renderer:");
			ImGui::Indent(16.0f);

			if (ImGui::Combo("Mode", (int*)&mRendererMode, "Raytraced AO\0Path Tracer\0Raytraced DI\0Debug\0\0"))
			{
				resetAccumulation();
			}

			if (mRendererMode == RendererModeType::Debug)
			{
				if (ImGui::Combo("Debug view", &mDebugView, "Base color\0UVs\0Shading normal\0Geometry normal\0Instance ID\0Geometry ID\0Triangle ID\0Roughness\0Metalness\0Opacity\0Motion vectors\0\0")) resetAccumulation();
			}

			if (mRendererMode == RendererModeType::RaytracedAO)
			{
				if (ImGui::SliderFloat("AO radius", &mAORadius, 0.1f, 200.0f)) resetAccumulation();
			}
			if (mRendererMode == RendererModeType::PathTracer)
			{
				if (ImGui::SliderInt("Max. Bounces", &mMaxBounces, 1, MAX_BOUNCES)) resetAccumulation();
				if (ImGui::Checkbox("Enable NEE", &mEnableNEE)) resetAccumulation();
				ImGui::BeginDisabled(!mEnableNEE);
				if (ImGui::Checkbox("Enable MIS", &mEnableMIS)) resetAccumulation();
				ImGui::EndDisabled();
			}

			ImGui::Spacing();

			ImGui::Indent(-16.0f);
			ImGui::Text("Sampling:");
			ImGui::Indent(16.0f);

			if (ImGui::Checkbox("Camera jitter", &mEnableAntiAliasing)) resetAccumulation();
			if (mRendererMode != RendererModeType::PathTracer && mRendererMode != RendererModeType::Debug)
				if (ImGui::Checkbox("TAA", &mEnableTAA)) resetAccumulation();
			ImGui::BeginDisabled(isTAAEnabled());
			if (ImGui::Checkbox("Accumulate", &mEnableAccumulation)) resetAccumulation();
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button("Reset Accumulation")) resetAccumulation();
			ImGui::Text("Accumulated Frames: %u", mAccumulatedFrames);

			if (isLightingMode())
			{
				ImGui::Spacing();
				ImGui::Indent(-16.0f);
				ImGui::Text("Output:");
				ImGui::Indent(16.0f);
				ImGui::Checkbox("Auto exposure", &mEnableAutoExposure);
				ImGui::SliderFloat("Exposure multiplier", &mExposure, 0.0f, 150.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
				ImGui::Checkbox("Tonemapping", &mEnableTonemapping);
			}

			ImGui::Indent(-16.0f);
		}
		ImGui::Separator();

		if (ImGui::CollapsingHeader("Scene information"))
		{
			constexpr double toMiB = 1.0 / (1024.0 * 1024.0);
			ImGui::Text("Geometry: %.2f MiB", mSceneMemory.geometry * toMiB);
			ImGui::Text("  %zu vertices", mScene.vertices.size());
			ImGui::Text("  %zu triangles", mScene.indices.size() / 3);
			ImGui::Text("Meshes:");
			ImGui::Text("  %zu geometries", mScene.geometries.size());
			ImGui::Text("  %zu instances", mScene.instances.size());
			ImGui::Text("Textures: %.2f MiB", mSceneMemory.textures * toMiB);
			ImGui::Text("  %zu images", mSceneImages.size());
			ImGui::Text("Triangle lights: %zu", mLights.size());
			ImGui::Text("BLAS: %.2f MiB (compacted)", mSceneMemory.blas * toMiB);
			ImGui::Text("TLAS: %.2f MiB", mSceneMemory.tlas * toMiB);
			ImGui::Text("Misc. buffers: %.2f MiB", mSceneMemory.other * toMiB);
			ImGui::Text("Total scene: %.2f MiB", (mSceneMemory.geometry + mSceneMemory.textures + mSceneMemory.blas + mSceneMemory.tlas + mSceneMemory.other) * toMiB);
		}
		if (ImGui::CollapsingHeader("Renderer information"))
		{
			constexpr double toMiB = 1.0 / (1024.0 * 1024.0);

			ImGui::Text("GBuffer: %.2f MiB", mRendererMemory.gbuffer * toMiB);
			ImGui::Text("TAA: %.2f MiB", mRendererMemory.taa * toMiB);
			ImGui::Text("Output and accumulation: %.2f MiB", mRendererMemory.output * toMiB);
			ImGui::Text("Auto exposure: %.2f MiB", mRendererMemory.exposure * toMiB);
			ImGui::Text("Swap-chain back buffers: %.2f MiB", mRendererMemory.backBuffers * toMiB);
			ImGui::Text("Other resources: %.2f MiB", mRendererMemory.other * toMiB);
			ImGui::Text("Total renderer: %.2f MiB", (mRendererMemory.gbuffer + mRendererMemory.taa + mRendererMemory.output + mRendererMemory.exposure + mRendererMemory.backBuffers + mRendererMemory.other) * toMiB);
		}
		ImGui::Separator();

		{
			ImGui::Text("Profiler:");
			ImGui::Indent(16.0f);
			ImGui::TextUnformatted(profiling.c_str());
			ImGui::Indent(-16.0f);
		}

		if (mReloadShaders)
		{
			ImGui::SetWindowFontScale(2.0f);
			ImGui::Text("RECOMPILING SHADERS!");
			ImGui::SetWindowFontScale(1.0f);
		}

		ImGui::End();
	}

	bool Update(HWND hwnd, const float elapsedTime)
	{
		std::string profiling = mProfiler[mCurrentFrameIndex].BeginFrame(mCmdList, mCmdQueue);

        if (mShowGUI)
        {
			drawGUI(profiling, elapsedTime);
        }

		// Keep the GUI and presentation active when there is no geometry to render.
		if (!hasSceneGeometry())
		{
			presentFrame(false);
			return true;
		}

		// Finish pending frames before replacing TAA resources.
		if (mTAA.GetMemoryUsage() && (!isTAAEnabled() || mShaderData.outputWidth != mFrameWidth || mShaderData.outputHeight != mFrameHeight)) waitForGPU(mFenceValue);

		// Ensure TAA
		if (isTAAEnabled())
		{
			mTAA.Prepare(mDevice, mFrameWidth, mFrameHeight, mAccumulationBuffer, mGBufferTextures[UINT(GBufferTexture::MotionBarycentrics)]);
		}
		else
		{
			mTAA.ReleaseResources();
		}

		mRendererMemory.taa = mTAA.GetMemoryUsage();

		// Update constant buffer
		{
			// Scene parameters
			mShaderData.outputWidth = mFrameWidth;
			mShaderData.outputHeight = mFrameHeight;
			mShaderData.sceneMin = mScene.sceneMin;
			mShaderData.sceneMax = mScene.sceneMax;
			mShaderData.sceneScale = glm::max(mScene.sceneMax - mScene.sceneMin, glm::vec3(1e-4f));
			
			// Rendering parameters
			mShaderData.aoRadius = mAORadius;
			mShaderData.exposureAdjustment = mExposure;
			mShaderData.enableTonemapping = mEnableTonemapping;
			mShaderData.enableAntiAliasing = mEnableAntiAliasing;
			mShaderData.maxBounces = mMaxBounces;
			mShaderData.enableNEE = mEnableNEE;
			mShaderData.enableMIS = mEnableMIS;
			mShaderData.enableTAA = isTAAEnabled();
			mShaderData.enableAutoExposure = mEnableAutoExposure && isLightingMode();
			mShaderData.resetAutoExposure = !mAutoExposureInitialized;
			mShaderData.outputAO = mRendererMode == RendererModeType::RaytracedAO;
			mShaderData.deltaTime = elapsedTime * 0.001f; // Frame time is supplied in milliseconds.
			mShaderData.debugView = uint(mDebugView);
			
			// Camera matrices
			glm::mat4 lookAt = glm::lookAtLH(glm::zero<glm::vec3>(), mCameraForward, mCameraUp);
			glm::mat4 position = glm::translate(glm::identity<glm::mat4>(), -mCameraPosition);
			glm::mat4 view = lookAt * position;
			glm::mat4 invView = glm::inverse(view);
			glm::mat4 normal = glm::transpose(invView);

			// Note: near and far plane settings have no effect in ray tracing, thanks to the way we construct primary rays
			const float nearPlane = 0.01f;
			const float farPlane = 1000.00f;
			const float fovY = glm::radians(mCameraFOV);
			glm::mat4 projection = glm::perspectiveFovLH(fovY, static_cast<float>(mFrameWidth), static_cast<float>(mFrameHeight), nearPlane, farPlane);

			// A new view or projection starts a fresh history.
			if (mShaderData.view != normal || mShaderData.proj != projection) mAccumulatedFrames = 0;

			// Motion history follows rendered frames, independently of accumulation resets.
			const glm::mat4 viewProj = projection * view;
			mShaderData.previousViewProj = (mShaderData.frameNumber == 0) ? viewProj : mShaderData.viewProj;
			mShaderData.viewProj = viewProj;

			mShaderData.view = normal;
			mShaderData.proj = projection;
			mShaderData.aspectRatio = float(mFrameWidth) / float(mFrameHeight);
			mShaderData.tanHalfFovY = glm::tan(fovY * 0.5f);

			if (!mEnableAccumulation || isTAAEnabled() || mAccumulatedFrames == UINT32_MAX) mAccumulatedFrames = 0;
			mShaderData.accumulatedFrames = ++mAccumulatedFrames;

			uploadConstantBuffer();

			mShaderData.frameNumber++;
		}

		// Setup root signature
		{
			ID3D12DescriptorHeap* ppHeaps[] = { mDescriptorHeap };
			mCmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
			mCmdList->SetComputeRootSignature(mGlobalRootSignature);
			mCmdList->SetComputeRootDescriptorTable(UINT(RootParameterIndex::CbvSrvUavs), mDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		}

		// Run G-Buffer Pass
		if (mRendererMode != RendererModeType::PathTracer)
		{
			PROFILE(L"G-Buffer");
			for (auto* texture : mGBufferTextures) transitionBarrier(texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			mGBufferPass.Dispatch(mCmdList, mFrameWidth, mFrameHeight);
			for (auto* texture : mGBufferTextures) transitionBarrier(texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}

		// Run rendering passes
		{
			if (mRendererMode == RendererModeType::RaytracedAO)
			{
				PROFILE(L"Raytraced AO");
				mRaytracedAOPass.Dispatch(mCmdList, mFrameWidth, mFrameHeight);
			}
			else if (mRendererMode == RendererModeType::PathTracer)
			{
				PROFILE(L"Path Tracer");
				mPathtracerPass.Dispatch(mCmdList, mFrameWidth, mFrameHeight);
			}
			else if (mRendererMode == RendererModeType::RaytracedDI)
			{
				PROFILE(L"Ray Traced DI");
				mRaytracedDIPass.Dispatch(mCmdList, mFrameWidth, mFrameHeight);
			}
			else if (mRendererMode == RendererModeType::Debug)
			{
				PROFILE(L"Debug visualization");
				mDebugPass.Dispatch(mCmdList, mFrameWidth, mFrameHeight, 8);
			}
		}

		if (isTAAEnabled())
		{
			PROFILE(L"TAA");
			mTAA.Dispatch(mCmdList, mAccumulationBuffer);
		}

		// Apply exposure and tonemapping after HDR accumulation or TAA
		if (mRendererMode != RendererModeType::Debug)
		{
			uavBarrier(mAccumulationBuffer);

			// TAA uses its own descriptor heap and root signature
			mCmdList->SetDescriptorHeaps(1, &mDescriptorHeap);
			mCmdList->SetComputeRootSignature(mGlobalRootSignature);
			mCmdList->SetComputeRootDescriptorTable(UINT(RootParameterIndex::CbvSrvUavs), mDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

			if (mShaderData.enableAutoExposure)
			{
				PROFILE(L"Auto exposure");
				
				// Exactly one 16x16 group builds and reduces the entire histogram
				mAutoExposurePass.Dispatch(mCmdList, 16, 16, 16);
				uavBarrier(mAutoExposureBuffer);
				mAutoExposureInitialized = true;
			}

			PROFILE(L"Tonemapping");
			mTonemappingPass.Dispatch(mCmdList, mFrameWidth, mFrameHeight, 8);
		}

		presentFrame(true);
		return true;
	}

	void presentFrame(bool copyOutput)
	{
		// Clear the screen
		D3D12_CPU_DESCRIPTOR_HANDLE destination = getCurrentBackBufferView();
		const glm::vec4 black = glm::vec4(0, 0, 0, 0);
		transitionBarrier(mBackBuffer[mCurrentFrameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
		mCmdList->ClearRenderTargetView(destination, &black.x, 0, nullptr);

		// Specify the buffers we are going to render to - destination (back buffer)
		D3D12_CPU_DESCRIPTOR_HANDLE depthStencilBufferViewHandle = mDsvHeap->GetCPUDescriptorHandleForHeapStart();
		mCmdList->OMSetRenderTargets(1, &destination, true, &depthStencilBufferViewHandle);

		// Copy the final output to the back buffer
		if (copyOutput)
		{
			transitionBarrier(mBackBuffer[mCurrentFrameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
			transitionBarrier(mOutputBuffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);

			// Copy Final output (inference result)
			{
				CD3DX12_TEXTURE_COPY_LOCATION dest(mBackBuffer[mCurrentFrameIndex]);
				CD3DX12_TEXTURE_COPY_LOCATION src(mOutputBuffer);
				CD3DX12_BOX box(0, 0, mFrameWidth, mFrameHeight);
				mCmdList->CopyTextureRegion(&dest, 0, 0, 0, &src, &box);
			}

			transitionBarrier(mBackBuffer[mCurrentFrameIndex], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
			transitionBarrier(mOutputBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}

		// Render ImGUI
		if (mShowGUI)
		{
			mCmdList->SetDescriptorHeaps(1, &imguiSrvDescHeap);
			ImGui::Render();
			ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), mCmdList);
		}

		transitionBarrier(mBackBuffer[mCurrentFrameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

		// End frame - process cmd. list and move to next frame
		mProfiler[mCurrentFrameIndex].EndFrame();
		submitCmdList();
		present();
		moveToNextFrame();
		resetCommandList();

		// Reload shaders here if needed
		if (mReloadShaders) {
			waitForGPU(mFenceValue); // Shader objects and descriptors may still be in use.
			mResourceUploader.Initialize(mDevice, mCmdQueue);
			createComputePasses();
			mResourceUploader.Finish();

			createResourceViews();
			mReloadShaders = false;
		}
	}

	void Cleanup() 
	{
		// Keep resources alive until all submitted rendering has completed
		if (mCmdQueue && mFence && mFenceEvent) finishSceneUpload();

		if (ImGui::GetCurrentContext())
		{
			if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplDX12_Shutdown();
			if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
			ImGui::DestroyContext();
		}

		SAFE_RELEASE(imguiSrvDescHeap);
		mImguiFreeDescriptors.clear();
		releaseSceneResources();
		mTAA.Cleanup();
		for (auto& profiler : mProfiler) profiler.Cleanup();
		for (auto*& buffer : mShaderDataCBUpload) SAFE_RELEASE(buffer);
		for (auto*& texture : mGBufferTextures) SAFE_RELEASE(texture);
		SAFE_RELEASE(mAccumulationBuffer);
		SAFE_RELEASE(mAutoExposureBuffer);
	}

private:

	void makeSceneList()
	{
		// Grab assets folder next to .exe
		std::filesystem::path assetsFolder = std::filesystem::path(utils::getExePath()) / L"assets";
		std::error_code error;

		// For development, fallback to hardcoded assets folder inside project sources
		if (!std::filesystem::is_directory(assetsFolder, error)) assetsFolder = std::filesystem::path(ASSETS_DIR);

		// Clear the state
		mSceneFiles.clear();
		mSceneNames.clear();
		mSelectedScene = 0;

		// Skip inaccessible directories and collect supported scene files recursively.
		error.clear();
		std::filesystem::recursive_directory_iterator entry(assetsFolder, std::filesystem::directory_options::skip_permission_denied, error), end;
		while (!error && entry != end)
		{
			std::error_code fileError;
			if (entry->is_regular_file(fileError))
			{
				const std::wstring path = entry->path().wstring();

				// Grab GLB, GLTF and OBJ scenes
				if (utils::hasExtension(path, L"gltf", L"glb") || utils::hasExtension(path, L"obj"))
				{
					mSceneFiles.push_back(path);
				}
			}
			entry.increment(error);
		}

		// Fail on failure
		if (error) utils::printMessage("Could not finish scanning scene directory: " + error.message() + "\n");

		// Sort the scenes for stable ordering
		std::sort(mSceneFiles.begin(), mSceneFiles.end());

		// Make a list of scene names for GUI
		for (const auto& path : mSceneFiles) mSceneNames.push_back(utils::wstringToString(std::filesystem::path(path).lexically_relative(assetsFolder).wstring()));
		
		// Insert empty scene to beginning
		mSceneFiles.insert(mSceneFiles.begin(), L"");
		mSceneNames.insert(mSceneNames.begin(), "Empty scene");

		// Keep a command-line scene selectable even when it is outside the assets folder.
		if (!mSceneFile.empty())
		{
			const std::wstring currentPath = utils::resolvePath(mSceneFile);
			for (size_t i = 1; i < mSceneFiles.size(); i++)
			{
				if (utils::resolvePath(mSceneFiles[i]) == currentPath)
				{
					mSelectedScene = int(i);
					return;
				}
			}
			mSelectedScene = int(mSceneFiles.size());
			mSceneFiles.push_back(mSceneFile);
			mSceneNames.push_back(utils::wstringToString(mSceneFile));
		}
	}

	bool loadScene()
	{
		HRESULT result = S_OK;

		// Load scene from either GLTF or OBJ file if specified
		if (!mSceneFile.empty())
		{
			if (utils::hasExtension(mSceneFile, L"glb", L"gltf"))
			{
				result = mScene.LoadGLTF(mSceneFile);
			}
			else if (utils::hasExtension(mSceneFile, L"obj"))
			{
				result = mScene.LoadOBJ(mSceneFile);
			}
			else
			{
				result = E_FAIL;
			}
		}

		// Scene loading attempted but failed
		if (FAILED(result))
		{
			utils::printMessage("Failed to load scene: " + utils::wstringToString(mSceneFile) + "\n");
			
			// Cleanup after failed loading
			mScene.Clear();
		}

		// Extract triangle lights
		mLights = mScene.ExtractLights();
		mShaderData.triangleLightCount = UINT(mLights.size());

		// Load default camera
		loadCamera(L"");

		// Load camera from file if specified
		if (SUCCEEDED(result) && !mCameraFile.empty())
		{
			loadCamera(mCameraFile);
		}

		return SUCCEEDED(result);
	}

	void createSceneResources()
	{
		// Create descriptor heap
		{
			createDescriptorHeap(mDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UINT(DescriptorHeapConstants::FixedDescriptorCount) + getSceneTextureDescriptorCount(), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &mDescriptorHeap);

			// Get the descriptor heap handle and increment size
			mCbvSrvUavDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}

		mResourceUploader.Initialize(mDevice, mCmdQueue, getSceneUploadCapacity());

		// Build BLAS/TLAS and upload scene geometry
		{
			buildSceneBuffers();
		}

		updateSceneMemory();
	}

	void updateRendererMemory()
	{
		auto allocationSize = [&](ID3D12Resource* resource) { return getResourceAllocationSize(mDevice, resource); };
		mRendererMemory = {};
		for (auto* texture : mGBufferTextures) mRendererMemory.gbuffer += allocationSize(texture);
		for (auto* buffer : mBackBuffer) mRendererMemory.backBuffers += allocationSize(buffer);
		mRendererMemory.taa = mTAA.GetMemoryUsage();
		mRendererMemory.output = allocationSize(mOutputBuffer) + allocationSize(mAccumulationBuffer);
		mRendererMemory.exposure = allocationSize(mAutoExposureBuffer);
		mRendererMemory.other = allocationSize(mShaderDataCB) + allocationSize(mDepthStencilBuffer);
		for (auto* buffer : mShaderDataCBUpload) mRendererMemory.other += allocationSize(buffer);
	}

	// Cache persistent scene resource allocations, including alignment; exclude staging, scratch and renderer resources.
	void updateSceneMemory()
	{
		auto allocationSize = [&](ID3D12Resource* resource) { return getResourceAllocationSize(mDevice, resource); };

		mSceneMemory = {};
		
		mSceneMemory.geometry = allocationSize(mVertexBuffer) + allocationSize(mIndexBuffer);
		
		for (auto* resource : mSceneImages) mSceneMemory.textures += allocationSize(resource);
		for (auto* resource : mBLASes) mSceneMemory.blas += allocationSize(resource);
		
		mSceneMemory.tlas = allocationSize(mTLAS);
		mSceneMemory.other = allocationSize(mInstanceDataBuffer) + allocationSize(mMaterialBuffer) + allocationSize(mTriangleLightsBuffer) + allocationSize(mTLASInstances) + allocationSize(mEmptyBuffer);
	}

	void finishSceneUpload()
	{
		// Copies must complete before executing transitions and acceleration-structure builds.
		mResourceUploader.Finish();
		submitCmdList();
		waitForGPU(mFenceValue);
		SAFE_RELEASE(mAccelerationStructureScratch);
		resetCommandList();
	}

	// The GPU and uploader must be idle before releasing scene resources.
	void releaseSceneResources()
	{
		mSceneMemory = {};
		SAFE_RELEASE(mTLAS);
		SAFE_RELEASE(mTLASInstances);
		for (auto*& resource : mBLASes) SAFE_RELEASE(resource);
		std::vector<ID3D12Resource*>().swap(mBLASes);
		SAFE_RELEASE(mAccelerationStructureScratch);
		SAFE_RELEASE(mVertexBuffer);
		SAFE_RELEASE(mIndexBuffer);
		SAFE_RELEASE(mInstanceDataBuffer);
		SAFE_RELEASE(mEmptyBuffer);
		SAFE_RELEASE(mTriangleLightsBuffer);
		SAFE_RELEASE(mMaterialBuffer);
		for (auto*& resource : mSceneImages) SAFE_RELEASE(resource);
		std::vector<ID3D12Resource*>().swap(mSceneImages);
		SAFE_RELEASE(mDescriptorHeap);

		// Release vector capacities too, so the next scene does not retain the previous scene's memory.
		mScene = Scene{};
		std::vector<TriangleLight>().swap(mLights);
		mShaderData.frameNumber = 0;
		mShaderData.triangleLightCount = 0;
		resetAccumulation();
		mAutoExposureInitialized = false;
		mLastMousePosition = glm::vec2(FLT_MAX);
	}

	bool hasSceneGeometry() const { return !mScene.instances.empty() && !mScene.geometries.empty() && !mScene.vertices.empty() && !mScene.indices.empty(); }

	bool isLightingMode() const { return mRendererMode == RendererModeType::RaytracedDI || mRendererMode == RendererModeType::PathTracer; }

	bool isTAAEnabled() const { return mEnableTAA && mRendererMode != RendererModeType::PathTracer && mRendererMode != RendererModeType::Debug; }

	void resetAccumulation()
	{
		mAccumulatedFrames = 0;
		mTAA.ResetHistory();
	}

	void initializeDx12(HWND hwnd) {

		for (int i = 0; i < kMaxFramesInFlight; i++) {
			mFenceValues[i] = 0;
			mBackBuffer[i] = nullptr;
			mCmdAlloc[i] = nullptr;
		}

		// Create DXGI factory
		HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&mDxgiFactory));
		utils::validate(hr, L"Error: failed to create DXGI factory!");

		const D3D_FEATURE_LEVEL kDx12FeatureLevel = D3D_FEATURE_LEVEL_12_1;

#ifdef _DEBUG
		// Enable the D3D12 debug layer.
		{
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&mDebugController))))
			{
				mDebugController->EnableDebugLayer();
			}
		}
#endif

		// Find a suitable adapter to run D3D
		bool foundAdapter = false;
		IDXGIAdapter1* tempAdapter = nullptr;
		DXGI_ADAPTER_DESC1 selectedAdapterDesc = {};

		for (UINT i = 0; SUCCEEDED(mDxgiFactory->EnumAdapters1(i, &tempAdapter)); i++)
		{
			bool foundHardwareAdapter = false;

			// Try to create this device
			ID3D12Device5* tempDevice;
			if (SUCCEEDED(D3D12CreateDevice(tempAdapter, kDx12FeatureLevel, _uuidof(ID3D12Device5), (void**)&tempDevice)))
			{
				// Query capabilities before selecting the adapter.
				const D3D12Capabilities capabilities = getDeviceCapabilities(tempDevice);
				DXGI_ADAPTER_DESC1 desc;

				// Grab features and description
				if (SUCCEEDED(tempAdapter->GetDesc1(&desc)))
				{
					if (capabilities.supportsRayTracing() && capabilities.shaderModel65)
					{
						selectedAdapterDesc = desc;
						mCapabilities = capabilities;
						foundAdapter = true;
						foundHardwareAdapter = !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
					}
				}

				SAFE_RELEASE(tempDevice);
			}

			SAFE_RELEASE(tempAdapter);

			// Break out of the loop if we found suitable non-software adapter, otherwise keep searching
			if (foundHardwareAdapter)
			{
				break;
			}
		}

		if (!foundAdapter)
		{
			utils::validate(E_FAIL, L"Error: no adapter supporting ray tracing and Shader Model 6.5 found!");
			return; // If validate doesn't already throw or terminate.
		}

		// Create D3D Device
		{
			HRESULT hr = mDxgiFactory->EnumAdapterByLuid(selectedAdapterDesc.AdapterLuid, IID_PPV_ARGS(&mAdapter));
			utils::validate(hr, L"Error: failed to enumerate selected adapter by luid!");

			hr = D3D12CreateDevice(mAdapter, kDx12FeatureLevel, _uuidof(ID3D12Device5), (void**)&mDevice);
			utils::validate(hr, L"Error: failed to create D3D device!");

			mAdapterName = selectedAdapterDesc.Description;
		}

		// Create command queue
		{
			D3D12_COMMAND_QUEUE_DESC desc = {};
			desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

			HRESULT hr = mDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&mCmdQueue));
			utils::validate(hr, L"Error: failed to create command queue!");
		}

		// Create a command allocator for each frame
		{
			for (UINT n = 0; n < kMaxFramesInFlight; n++)
			{
				HRESULT hr = mDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mCmdAlloc[n]));
				utils::validate(hr, L"Error: failed to create the command allocator!");
			}
		}

		// Create Command List
		{
			// Create the command list
			HRESULT hr = mDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mCmdAlloc[mCurrentFrameIndex], nullptr, IID_PPV_ARGS(&mCmdList));
			hr = mCmdList->Close();
			utils::validate(hr, L"Error: failed to create the command list!");

			resetCommandList();
		}

		// Create fence
		{
			HRESULT hr = mDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mFence));
			utils::validate(hr, L"Error: failed to create fence!");

			// Create an event handle to use for frame synchronization
			mFenceEvent = CreateEventEx(nullptr, FALSE, FALSE, EVENT_ALL_ACCESS);
			if (mFenceEvent == nullptr)
			{
				hr = HRESULT_FROM_WIN32(GetLastError());
				utils::validate(hr, L"Error: failed to create fence event!");
			}
		}

		// Create swap chain
		{
			mCapabilities.supportsTearing = supportsTearing(mDxgiFactory);

			// Describe the swap chain
			DXGI_SWAP_CHAIN_DESC1 desc = {};
			desc.BufferCount = kMaxFramesInFlight;
			desc.Width = mFrameWidth;
			desc.Height = mFrameHeight;
			desc.Format = mBackBufferFormat;
			desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Flags = mCapabilities.supportsTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

			IDXGISwapChain1* tempSwapChain;

			// Create the swap chain
			hr = mDxgiFactory->CreateSwapChainForHwnd(mCmdQueue, hwnd, &desc, 0, 0, &tempSwapChain);
			utils::validate(hr, L"Error: failed to create swap chain!");

			if (mCapabilities.supportsTearing)
			{
				// When tearing support is enabled we will handle ALT+Enter key presses in the
				// window message loop rather than let DXGI handle it by calling SetFullscreenState.
				mDxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
				utils::validate(hr, L"Error: failed to make window association!");
			}

			// Get the swap chain interface
			hr = tempSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&mSwapChain));
			utils::validate(hr, L"Error: failed to cast swap chain!");

			SAFE_RELEASE(tempSwapChain);
			mCurrentFrameIndex = mSwapChain->GetCurrentBackBufferIndex();
		}

		// Create RTV heap
		{
			createDescriptorHeap(mDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxFramesInFlight, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &mRtvHeap);

			mRtvDescSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		}

		// Create DSV heap
		{
			createDescriptorHeap(mDevice, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, &mDsvHeap);
		}

		// Create back buffer
		{
			HRESULT hr;
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();

			// Create a RTV for each back buffer
			for (unsigned int n = 0; n < kMaxFramesInFlight; n++)
			{
				hr = mSwapChain->GetBuffer(n, IID_PPV_ARGS(&mBackBuffer[n]));
				utils::validate(hr, L"Error: failed to get swap chain buffer!");

				mDevice->CreateRenderTargetView(mBackBuffer[n], nullptr, rtvHandle);

				rtvHandle.ptr += mRtvDescSize;
			}
		}

		// Create depth stencil buffer
		{
			// Create the depth/stencil buffer and view.
			D3D12_RESOURCE_DESC depthStencilDesc;
			depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			depthStencilDesc.Alignment = 0;
			depthStencilDesc.Width = mFrameWidth;
			depthStencilDesc.Height = mFrameHeight;
			depthStencilDesc.DepthOrArraySize = 1;
			depthStencilDesc.MipLevels = 1;
			depthStencilDesc.Format = mDepthBufferFormat;
			depthStencilDesc.SampleDesc.Count = 1;
			depthStencilDesc.SampleDesc.Quality = 0;
			depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
			depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

			D3D12_CLEAR_VALUE optClear;
			optClear.Format = mDepthBufferFormat;
			optClear.DepthStencil.Depth = 1.0f;
			optClear.DepthStencil.Stencil = 0;

			mDevice->CreateCommittedResource(
				&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
				D3D12_HEAP_FLAG_NONE,
				&depthStencilDesc,
				D3D12_RESOURCE_STATE_COMMON,
				&optClear,
				__uuidof(ID3D12Resource), (void**)&mDepthStencilBuffer);

			// Create descriptor to mip level 0 of entire resource	using the format of the resource.
			D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc;
			dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
			dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			dsvDesc.Format = mDepthBufferFormat;
			dsvDesc.Texture2D.MipSlice = 0;

			CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mDsvHeap->GetCPUDescriptorHandleForHeapStart());

			mDevice->CreateDepthStencilView(mDepthStencilBuffer, &dsvDesc, hDescriptor);

			// Transition the resource from its initial state to be used as a depth buffer.
			mCmdList->ResourceBarrier(1,
				&CD3DX12_RESOURCE_BARRIER::Transition(
					mDepthStencilBuffer,
					D3D12_RESOURCE_STATE_COMMON,
					D3D12_RESOURCE_STATE_DEPTH_WRITE));
		}

		// Create root signature
		{
			mGlobalRootSignature = createGlobalRootSignature();
		}

		createSceneResources();

		// Create shaders and compute passes
		{
			createComputePasses();
		}

		// Create output buffer
		{
			createTexture2D(mDevice, mFrameWidth, mFrameHeight, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &mOutputBuffer, L"Output");

			// Buffer for accumulating many frames
			createTexture2D(mDevice, mFrameWidth, mFrameHeight, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &mAccumulationBuffer, L"Accumulation");
			resetAccumulation();
		}

		// Persistent exposure scalar
		{
			createTexture2D(mDevice, 1, 1, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &mAutoExposureBuffer, L"Auto exposure");
			mAutoExposureInitialized = false;
		}

		// Create constant buffer
		{
			mShaderDataCBSize = ALIGN(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, sizeof(mShaderData));
			createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, mShaderDataCBSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &mShaderDataCB);

			UINT64 uploadBufferSize = GetRequiredIntermediateSize(mShaderDataCB, 0, 1);
			uploadBufferSize = ALIGN(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, uploadBufferSize);
			// CPU uploads must remain unchanged until their frame completes.
			for (auto*& buffer : mShaderDataCBUpload) createBuffer(mDevice, D3D12_HEAP_TYPE_UPLOAD, 0, uploadBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, &buffer);
		}

		// Create G-buffer textures
		for (UINT i = 0; i < UINT(GBufferTexture::Count); i++)
		{
			createTexture2D(mDevice, mFrameWidth, mFrameHeight, kGBufferFormats[i], D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &mGBufferTextures[i], L"G-buffer " + std::to_wstring(i));
		}

		// Fill descriptor heap
		{
			createResourceViews();
		}
	}

	void loadCamera(std::wstring cameraFile)
	{
		if (cameraFile == L"")
		{
			if (!mScene.cameras.empty())
			{
				// Use camera from scene file (GLTF)
				const auto& camera = mScene.cameras.front();
				mCameraPosition = camera.position;
				mCameraForward = camera.forward;
				mCameraUp = camera.up;
				mCameraRight = glm::normalize(glm::cross(mCameraUp, mCameraForward));
				mCameraUp = glm::normalize(glm::cross(mCameraForward, mCameraRight));
				mCameraFOV = camera.fovY;
			}
			else
			{
				// Create default camera
				mCameraForward = glm::vec3(0, 0, 1);
				mCameraRight = glm::vec3(1, 0, 0);
				mCameraUp = glm::vec3(0, 1, 0);
				mCameraPosition = glm::vec3(0.0f);
				mCameraFOV = 42.0f;
				if (!hasSceneGeometry()) return;

				mCameraPosition.x = (mScene.sceneMax.x + mScene.sceneMin.x) * 0.5f;
				mCameraPosition.y = (mScene.sceneMax.y + mScene.sceneMin.y) * 0.5f;
				mCameraPosition.z = (mScene.sceneMax.z + mScene.sceneMin.z) * 0.5f;

				// Place the camera so that it views the whole scene within its field of view
				mCameraPosition.z = -mScene.sceneMax.z;
				mCameraPosition.z -= ((mScene.sceneMax.y - mScene.sceneMin.y) * 0.55f) / std::tan(glm::radians<float>(mCameraFOV) / 2.0f);
			}
		}
		else
		{
			// Read camera file
			std::wstring line;
			std::wifstream inputfile(cameraFile);

			if (inputfile.is_open()) {

				for (int i = 0; i < 5; i++)
				{
					getline(inputfile, line);
					std::wistringstream iss(line);

					switch (i)
					{
					case 0:
						iss >> mCameraPosition.x >> mCameraPosition.y >> mCameraPosition.z;
						break;
					case 1:
						iss >> mCameraForward.x >> mCameraForward.y >> mCameraForward.z;
						break;
					case 2:
						iss >> mCameraRight.x >> mCameraRight.y >> mCameraRight.z;
						break;
					case 3:
						iss >> mCameraUp.x >> mCameraUp.y >> mCameraUp.z;
						break;
					case 4:
						iss >> mCameraFOV;
						break;
					}
				}
				
				inputfile.close();
			}
		}
	}

    void createResourceViews()
	{
		// Create the ShaderData CBV
		{
			createConstantBufferView(mDevice, mShaderDataCB, mShaderDataCBSize, getDescriptorHandle(UINT(DescriptorHeapConstants::ShaderDataCB)));
		}

		// Create the DXR output buffer UAV
		{
			createTexture2DUAV(mDevice, mOutputBuffer, DXGI_FORMAT_UNKNOWN, getDescriptorHandle(UINT(DescriptorHeapConstants::Output)));

			createTexture2DUAV(mDevice, mAccumulationBuffer, DXGI_FORMAT_R32G32B32A32_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::AccumulationBuffer)));

			createTexture2DUAV(mDevice, mAutoExposureBuffer, DXGI_FORMAT_R32_FLOAT, getDescriptorHandle(UINT(DescriptorHeapConstants::AutoExposure)));
		}

		// G-buffer UAVs for primary hits and SRVs for all subsequent passes.
		for (UINT i = 0; i < UINT(GBufferTexture::Count); i++)
		{
			createTexture2DUAV(mDevice, mGBufferTextures[i], kGBufferFormats[i], getDescriptorHandle(UINT(DescriptorHeapConstants::GBufferPositionOutput) + i));

			createTexture2DSRV(mDevice, mGBufferTextures[i], kGBufferFormats[i], getDescriptorHandle(UINT(DescriptorHeapConstants::GBufferPosition) + i));
		}

		
	}

	void createComputePasses() {

		resetAccumulation();

		const std::wstring shaderFolder = utils::getExePath() + L"shaders\\";

		std::vector<std::wstring> compilerFlags;

		compilerFlags.push_back((std::wstring(L"/D PI=") + std::to_wstring(glm::pi<float>())).c_str());

		// Exclude inline ray queries from shaders on devices without DXR Tier 1.1 support.
		compilerFlags.push_back(mCapabilities.supportsInlineRayTracing() ? L"/D SUPPORTS_INLINE_RAY_TRACING=1" : L"/D SUPPORTS_INLINE_RAY_TRACING=0");

        // Process compiler flags to an array of string pointers
        std::vector<LPCWSTR> flagsPointers;
        flagsPointers.reserve(compilerFlags.size());
        for (const std::wstring& flag : compilerFlags) {
            flagsPointers.push_back(flag.c_str());
        }

		mTAA.Compile(mDevice, mShaderCompiler, shaderFolder + L"TAA.hlsl");

		// Compile exposure metering at startup and on explicit shader reloads.
		{
			const std::wstring shaderFile = shaderFolder + L"AutoExposure.hlsl";
			IDxcBlob* shaderBlob = mShaderCompiler.CompileShader(shaderFile.c_str(), L"AutoExposure", L"cs_6_5", flagsPointers);
			mAutoExposurePass.Initialize(mDevice, mGlobalRootSignature, *shaderBlob, L"Auto exposure");
			SAFE_RELEASE(shaderBlob);
		}

		// Compile the shared final HDR-to-display pass.
		{
			const std::wstring shaderFile = shaderFolder + L"Tonemapping.hlsl";
			IDxcBlob* shaderBlob = mShaderCompiler.CompileShader(shaderFile.c_str(), L"Tonemapping", L"cs_6_5", flagsPointers);
			mTonemappingPass.Initialize(mDevice, mGlobalRootSignature, *shaderBlob, L"Tonemapping");
			SAFE_RELEASE(shaderBlob);
		}

		// Compile debug visualization at startup and on explicit shader reloads.
		{
			const std::wstring debugShaderFile = shaderFolder + L"Debug.hlsl";
			IDxcBlob* shaderBlob = mShaderCompiler.CompileShader(debugShaderFile.c_str(), L"DebugVisualization", L"cs_6_5", flagsPointers);
			mDebugPass.Initialize(mDevice, mGlobalRootSignature, *shaderBlob, L"Debug visualization");
		}

		// Compile G-Buffer pass
		{
			const std::wstring shaderFile = shaderFolder + L"GBufferPass.hlsl";
			IDxcBlob* shaderBlob = mShaderCompiler.CompileShader(shaderFile.c_str(), L"", L"lib_6_5", flagsPointers);
			mGBufferPass.Initialize(mDevice, mCmdList, mResourceUploader, L"RayGenGBuffer", mGlobalRootSignature, *shaderBlob);
			SAFE_RELEASE(shaderBlob);
		}

		// Compile raytraced AO pass
		{
			const std::wstring shaderFile = shaderFolder + L"RaytracedAO.hlsl";
			IDxcBlob* shaderBlob = mShaderCompiler.CompileShader(shaderFile.c_str(), L"", L"lib_6_5", flagsPointers);
			mRaytracedAOPass.Initialize(mDevice, mCmdList, mResourceUploader, L"RayGenRaytracedAO", mGlobalRootSignature, *shaderBlob);
			SAFE_RELEASE(shaderBlob);
		}

		// Compile path tracing pass
		{
			const std::wstring shaderFile = shaderFolder + L"PathTracer.hlsl";
			IDxcBlob* shaderBlob = mShaderCompiler.CompileShader(shaderFile.c_str(), L"", L"lib_6_5", flagsPointers);
			mPathtracerPass.Initialize(mDevice, mCmdList, mResourceUploader, L"RayGenPathTracer", mGlobalRootSignature, *shaderBlob);
			SAFE_RELEASE(shaderBlob);
		}

		// Compile raytraced DI pass
		{
			const std::wstring shaderFile = shaderFolder + L"RaytracedDI.hlsl";
			IDxcBlob* shaderBlob = mShaderCompiler.CompileShader(shaderFile.c_str(), L"", L"lib_6_5", flagsPointers);
			mRaytracedDIPass.Initialize(mDevice, mCmdList, mResourceUploader, L"RayGenRaytracedDI", mGlobalRootSignature, *shaderBlob);
			SAFE_RELEASE(shaderBlob);
		}

	}

	void transitionBarrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) 
	{
		d3d12Helpers::transitionBarrier(mCmdList, resource, from, to);
	}

	void uavBarrier(ID3D12Resource* resource) {
		d3d12Helpers::uavBarrier(mCmdList, resource);
	}

	D3D12_CPU_DESCRIPTOR_HANDLE getDescriptorHandle(UINT index) {
		D3D12_CPU_DESCRIPTOR_HANDLE handle = mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
		handle.ptr += (mCbvSrvUavDescSize * index);
		return handle;
	}

	UINT getSceneTextureDescriptorCount() const
	{
		return std::max(1u, UINT(mScene.textures.size()));
	}

	void createEmptyBuffer()
	{
		constexpr UINT64 emptyBufferSize = 256;
		createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, emptyBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &mEmptyBuffer, L"Empty Buffer");
	}

	void createTriangleLightBuffer()
	{
		if (mLights.empty())
		{
			createStructuredBufferSRV(mDevice, mEmptyBuffer, 1, sizeof(TriangleLight), getDescriptorHandle(UINT(DescriptorHeapConstants::TriangleLightsBuffer)));
			return;
		}

		const UINT64 bufferSize = UINT64(mLights.size()) * sizeof(TriangleLight);
		createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &mTriangleLightsBuffer, L"Triangle Lights");

		mResourceUploader.Upload(mLights.data(), mTriangleLightsBuffer);
		transitionBarrier(mTriangleLightsBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		createStructuredBufferSRV(mDevice, mTriangleLightsBuffer, UINT(mLights.size()), sizeof(TriangleLight), getDescriptorHandle(UINT(DescriptorHeapConstants::TriangleLightsBuffer)));
	}

	void createMaterialBuffer()
	{
		// Materials are addressed by instance ID, prepare such buffer
		std::vector<Material> materials(mScene.instances.size());
		for (size_t i = 0; i < mScene.instances.size(); i++)
		{
			materials[i] = mScene.materials[mScene.instances[i].materialIndex];
		}

		const UINT64 bufferSize = UINT64(materials.size()) * sizeof(Material);
		createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &mMaterialBuffer, L"Materials");

		mResourceUploader.Upload(materials.data(), mMaterialBuffer);
		transitionBarrier(mMaterialBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

		createStructuredBufferSRV(mDevice, mMaterialBuffer, UINT(materials.size()), sizeof(Material), getDescriptorHandle(UINT(DescriptorHeapConstants::MaterialBuffer)));
	}

	void createSceneTextures()
	{
		if (mScene.textures.empty())
		{
			createTexture2DSRV(mDevice, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, getDescriptorHandle(UINT(DescriptorHeapConstants::SceneTexturesStart)));
			return;
		}

		// Allocate and upload each unique image
		mSceneImages.resize(mScene.images.size());
		for (size_t imageIndex = 0; imageIndex < mScene.images.size(); imageIndex++)
		{
			Scene::Image& image = mScene.images[imageIndex];

			// Typeless resource; each view specifies whether sampling is sRGB or linear.
			createTexture2D(mDevice, image.width, image.height, DXGI_FORMAT_R8G8B8A8_TYPELESS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &mSceneImages[imageIndex], L"Scene Image " + std::to_wstring(imageIndex));

			mResourceUploader.UploadTexture(image.data(), mSceneImages[imageIndex]);
			
			// Release the image after upload
			image.pixels.reset();

			// Transition texture for reading
			transitionBarrier(mSceneImages[imageIndex], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}

		// Create SRVs to textures
		for (size_t textureIndex = 0; textureIndex < mScene.textures.size(); textureIndex++)
		{
			const Scene::Texture& texture = mScene.textures[textureIndex];
			const DXGI_FORMAT format = texture.isSrgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; //< use sRGB or linear for the view
			
			createTexture2DSRV(mDevice, mSceneImages[texture.imageIndex], format, getDescriptorHandle(UINT(DescriptorHeapConstants::SceneTexturesStart) + UINT(textureIndex)));
		}
	}

	// Figure out how much memory we need for GPU uploads
	UINT64 getSceneUploadCapacity() const
	{
		// Small allowance for the four shader tables created after scene loading.
		UINT64 bytes = 64 * 1024;

		// Helper to add bytes until max upload staging buffer budget
		auto addBytesWithinBudget  = [&](UINT64 count) { bytes += std::min(count, kUploadStagingBufferBudget - bytes); };

		// Include scene data
		addBytesWithinBudget (UINT64(mScene.vertices.size()) * sizeof(Vertex));
		addBytesWithinBudget (UINT64(mScene.indices.size()) * sizeof(glm::uint));
		addBytesWithinBudget (UINT64(mScene.instances.size()) * (sizeof(InstanceData) + sizeof(Material) + sizeof(D3D12_RAYTRACING_INSTANCE_DESC)));
		addBytesWithinBudget (UINT64(mLights.size()) * sizeof(TriangleLight));

		// Include unique image uploads (sampling views do not need separate copies)
		UINT64 largestRow = 0;
		for (const auto& image : mScene.images)
		{
			const UINT64 rowPitch = ALIGN(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, UINT64(image.width) * 4);
			largestRow = std::max(largestRow, rowPitch);
			
			// Include worst-case placement padding after preceding buffer/texture data.
			addBytesWithinBudget (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
			addBytesWithinBudget (rowPitch * image.height);
		}

		// Be flexible and support large texture row even if it's over max budget
		return std::max(bytes, largestRow);
	}

	void buildSceneBuffers()
	{
		createEmptyBuffer();

		// Empty scenes need valid descriptors, but no geometry uploads or acceleration structures.
		if (!hasSceneGeometry())
		{
			auto createEmptyView = [&](DescriptorHeapConstants descriptor, UINT stride)
			{
				createStructuredBufferSRV(mDevice, mEmptyBuffer, 1, stride, getDescriptorHandle(UINT(descriptor)));
			};
			createEmptyView(DescriptorHeapConstants::VertexBuffer, sizeof(Vertex));
			createEmptyView(DescriptorHeapConstants::IndexBuffer, sizeof(glm::uint));
			createEmptyView(DescriptorHeapConstants::InstanceDataBuffer, sizeof(InstanceData));
			createEmptyView(DescriptorHeapConstants::MaterialBuffer, sizeof(Material));

			createAccelerationStructureSRV(mDevice, nullptr, getDescriptorHandle(UINT(DescriptorHeapConstants::SceneTLAS)));

			createTriangleLightBuffer();
			createSceneTextures();
			return;
		}

		// Upload vertex and index buffers
		{
			createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, sizeof(Vertex) * mScene.vertices.size(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &mVertexBuffer, L"Vertex Buffer");
			mResourceUploader.Upload(mScene.vertices.data(), mVertexBuffer);
			transitionBarrier(mVertexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, sizeof(glm::uint) * mScene.indices.size(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &mIndexBuffer, L"Index Buffer");
			mResourceUploader.Upload(mScene.indices.data(), mIndexBuffer);
			transitionBarrier(mIndexBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

			// Create the SRV for vertex and index buffer
			createStructuredBufferSRV(mDevice, mVertexBuffer, UINT(mScene.vertices.size()), sizeof(Vertex), getDescriptorHandle(UINT(DescriptorHeapConstants::VertexBuffer)));

			createStructuredBufferSRV(mDevice, mIndexBuffer, UINT(mScene.indices.size()), sizeof(glm::uint), getDescriptorHandle(UINT(DescriptorHeapConstants::IndexBuffer)));
		}

		// Upload instance data
		{
			std::vector<InstanceData> instanceData(mScene.instances.size());
			for (size_t i = 0; i < mScene.instances.size(); i++)
			{
				const Scene::Instance& instance = mScene.instances[i];
				const Scene::Geometry& geometry = mScene.geometries[instance.geometryIndex];
				instanceData[i] = { geometry.vertexOffset, geometry.indexOffset };
			}
			createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, sizeof(InstanceData) * instanceData.size(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &mInstanceDataBuffer, L"Instance Data");
			mResourceUploader.Upload(instanceData.data(), mInstanceDataBuffer);
			transitionBarrier(mInstanceDataBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			
			createStructuredBufferSRV(mDevice, mInstanceDataBuffer, UINT(instanceData.size()), sizeof(InstanceData), getDescriptorHandle(UINT(DescriptorHeapConstants::InstanceDataBuffer)));
		}

		// Upload triangle lights, materials and scene textures
		createTriangleLightBuffer();
		createMaterialBuffer();
		createSceneTextures();

		// Prepare TLAS instance buffer
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
		createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * mScene.instances.size(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &mTLASInstances, L"TLAS Instances");

		// Query TLAS scratch requirements before allocating shared BLAS/TLAS scratch memory.
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
		tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlasInputs.InstanceDescs = mTLASInstances->GetGPUVirtualAddress();
		tlasInputs.NumDescs = UINT(mScene.instances.size());
		tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

		mDevice->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);

		// Build BLASes
		{
			// Helper to describe Dx12 Geometry object for our geometry
			auto makeGeometryDesc = [&](const Scene::Geometry& geometry)
				{
					// Description of geometry being build and BLAS type requested
					D3D12_RAYTRACING_GEOMETRY_DESC desc = {};
					desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
					desc.Triangles.VertexBuffer.StartAddress = mVertexBuffer->GetGPUVirtualAddress() + UINT64(geometry.vertexOffset) * sizeof(Vertex);
					desc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
					desc.Triangles.VertexCount = geometry.vertexCount;
					desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
					desc.Triangles.IndexBuffer = mIndexBuffer->GetGPUVirtualAddress() + UINT64(geometry.indexOffset) * sizeof(glm::uint);
					desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
					desc.Triangles.IndexCount = geometry.indexCount;
					desc.Flags = geometry.isOpaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
					return desc;
				};

			// Figure out how much memory we need to build BLASes
			UINT64 temporaryBytes = 0;
			UINT64 scratchBytes = tlasInfo.ScratchDataSizeInBytes;
			UINT64 totalScratchBytes = 0;
			UINT64 totalResultBytes = 0;

			const auto buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
			
			std::vector<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO> blasInfo(mScene.geometries.size());
			mBLASes.resize(mScene.geometries.size());

			for (size_t i = 0; i < mScene.geometries.size(); i++)
			{
				D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = makeGeometryDesc(mScene.geometries[i]);
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
				inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				inputs.pGeometryDescs = &geometryDesc;
				inputs.NumDescs = 1;
				inputs.Flags = buildFlags;
				mDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &blasInfo[i]);
				scratchBytes = std::max(scratchBytes, blasInfo[i].ScratchDataSizeInBytes);
				temporaryBytes = std::max(temporaryBytes, blasInfo[i].ResultDataMaxSizeInBytes);
				
				// Saturate totals at their budgets
				totalScratchBytes += std::min<UINT64>(ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasInfo[i].ScratchDataSizeInBytes), kBuildScratchBudget - totalScratchBytes);
				totalResultBytes += std::min<UINT64>(ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasInfo[i].ResultDataMaxSizeInBytes), kTemporaryBLASBudget - totalResultBytes);
			}

			scratchBytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, std::max<UINT64>(256, std::max(scratchBytes, totalScratchBytes)));
			temporaryBytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, std::max<UINT64>(256, std::max(temporaryBytes, totalResultBytes)));
			createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, scratchBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &mAccelerationStructureScratch, L"AS Scratch");

			// Allocate memory for building BLASes in batches
			ID3D12Resource* compactedSizes = nullptr;
			ID3D12Resource* temporaryBLAS = nullptr;
			ID3D12Resource* sizesReadback = nullptr;
			
			const UINT64 sizesBytes = std::max<UINT64>(1, mBLASes.size()) * sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);
			
			createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, temporaryBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &temporaryBLAS, L"Temporary BLAS Batch");
			createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, sizesBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, &compactedSizes, L"BLAS Compacted Sizes");
			createBuffer(mDevice, D3D12_HEAP_TYPE_READBACK, 0, sizesBytes, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, &sizesReadback, L"BLAS Sizes Readback");

			// BLAS batches execute now, so queue every pending geometry/texture copy first.
			mResourceUploader.Flush();
			for (size_t batchBegin = 0; batchBegin < mBLASes.size();)
			{
				UINT64 scratchOffset = 0;
				UINT64 resultOffset = 0;
				size_t batchEnd = batchBegin;

				// return buffers to required state after each submitted batch.
				transitionBarrier(compactedSizes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				for (; batchEnd < mBLASes.size(); batchEnd++)
				{
					const UINT64 buildScratchBytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasInfo[batchEnd].ScratchDataSizeInBytes);
					const UINT64 buildResultBytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasInfo[batchEnd].ResultDataMaxSizeInBytes);
					if (buildScratchBytes > scratchBytes - scratchOffset || buildResultBytes > temporaryBytes - resultOffset) break;

					D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = makeGeometryDesc(mScene.geometries[batchEnd]);

					D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
					build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
					build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
					build.Inputs.pGeometryDescs = &geometryDesc;
					build.Inputs.NumDescs = 1;
					build.Inputs.Flags = buildFlags;
					build.ScratchAccelerationStructureData = mAccelerationStructureScratch->GetGPUVirtualAddress() + scratchOffset;
					build.DestAccelerationStructureData = temporaryBLAS->GetGPUVirtualAddress() + resultOffset;

					// Build the BLAS
					D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuild = {};
					postbuild.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
					postbuild.DestBuffer = compactedSizes->GetGPUVirtualAddress() + (batchEnd - batchBegin) * sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);
					mCmdList->BuildRaytracingAccelerationStructure(&build, 1, &postbuild);

					scratchOffset += buildScratchBytes;
					resultOffset += buildResultBytes;
				}

				// Read back this batch's sizes before allocating its final BLAS buffers.
				uavBarrier(temporaryBLAS);
				transitionBarrier(compactedSizes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
				const UINT64 batchSizesBytes = (batchEnd - batchBegin) * sizeof(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC);
				mCmdList->CopyBufferRegion(sizesReadback, 0, compactedSizes, 0, batchSizesBytes);
				
				submitCmdList();
				waitForGPU(mFenceValue);
				resetCommandList();

				// Map the compacted sizes to CPU
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE_DESC* sizes = nullptr;
				const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(batchSizesBytes) };
				utils::validate(sizesReadback->Map(0, &readRange, reinterpret_cast<void**>(&sizes)), L"Failed to map BLAS sizes readback!");
				
				// Create final compacted buffers for BLASes
				resultOffset = 0;
				for (size_t i = batchBegin; i < batchEnd; i++)
				{
					const UINT64 bytes = ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, sizes[i - batchBegin].CompactedSizeInBytes);
					createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &mBLASes[i], L"Scene BLAS");
					mCmdList->CopyRaytracingAccelerationStructure(mBLASes[i]->GetGPUVirtualAddress(), temporaryBLAS->GetGPUVirtualAddress() + resultOffset, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
					resultOffset += ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, blasInfo[i].ResultDataMaxSizeInBytes);
				}
				const D3D12_RANGE writtenRange = { 0, 0 };
				sizesReadback->Unmap(0, &writtenRange);

				// Order compaction before the next batch reuses temporary memory.
				uavBarrier(nullptr);
				if (batchEnd == mBLASes.size())
				{
					// Intermediate compaction is submitted with the next build batch; wait here before releasing temporary buffers.
					submitCmdList();
					waitForGPU(mFenceValue);
					resetCommandList();
				}

				// Move to next batch
				batchBegin = batchEnd;
			}

			SAFE_RELEASE(sizesReadback);
			SAFE_RELEASE(compactedSizes);
			SAFE_RELEASE(temporaryBLAS);
		}

		// Build TLAS
		{

			// Build the instance list for the TLAS
			std::vector<D3D12_RAYTRACING_INSTANCE_DESC> gpuInstances(mScene.instances.size());
			{
				for (size_t i = 0; i < mScene.instances.size(); i++)
				{
					const auto& sceneInstance = mScene.instances[i];

					auto& desc = gpuInstances[i];
					desc.InstanceID = UINT(i);
					desc.InstanceContributionToHitGroupIndex = 0;
					desc.InstanceMask = 0xFF;
					desc.AccelerationStructure = mBLASes[sceneInstance.geometryIndex]->GetGPUVirtualAddress();
					desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
					if (mScene.materials[sceneInstance.materialIndex].alphaMode == ALPHA_MODE_OPAQUE)
					{
						desc.Flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
					}

					const glm::mat4 transposed = glm::transpose(sceneInstance.transform);
					memcpy(desc.Transform, &transposed[0][0], sizeof(desc.Transform));
				}
			
				mResourceUploader.Upload(gpuInstances.data(), mTLASInstances);
				transitionBarrier(mTLASInstances, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			}

			// Reuse the prebuild query and shared scratch buffer for the TLAS.
			createBuffer(mDevice, D3D12_HEAP_TYPE_DEFAULT, 0, ALIGN(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT, tlasInfo.ResultDataMaxSizeInBytes), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, &mTLAS, L"TLAS");
			
			// Build the TLAS
			{
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
				build.Inputs = tlasInputs;
				build.ScratchAccelerationStructureData = mAccelerationStructureScratch->GetGPUVirtualAddress();
				build.DestAccelerationStructureData = mTLAS->GetGPUVirtualAddress();
				mCmdList->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
				uavBarrier(mTLAS);
			}

			// Create the SRV for TLAS
			{
				createAccelerationStructureSRV(mDevice, mTLAS, getDescriptorHandle(UINT(DescriptorHeapConstants::SceneTLAS)));
			}
		}
	}

	/**
	* This enum specifies a layout of shader resources
	*/
	enum class DescriptorHeapConstants {

		// List of resources declared in the shader, as they appear in the descriptors heap
		ShaderDataCB = 0,
		Output,
		GBufferPositionOutput,
		GBufferNormalsOutput,
		GBufferUVOutput,
		GBufferMotionBarycentricsOutput,
		GBufferIDsOutput,
		AccumulationBuffer,
		AutoExposure,
		SceneTLAS,
		VertexBuffer,
		IndexBuffer,
		InstanceDataBuffer,
		TriangleLightsBuffer,
		MaterialBuffer,
		GBufferPosition,
		GBufferNormals,
		GBufferUV,
		GBufferMotionBarycentrics,
		GBufferIDs,
		FixedDescriptorCount = GBufferIDs + 1,
		SceneTexturesStart = FixedDescriptorCount,

		// Constant buffer range
		CBStart = ShaderDataCB,
		CBEnd = ShaderDataCB,
		CBTotal = CBEnd - CBStart + 1,

		// UAV space 0 range
		UAV0Start = Output,
		UAV0End = AutoExposure,
		UAV0Total = UAV0End - UAV0Start + 1,

		// SRV space 0 range
		SRV0Start = SceneTLAS,
		SRV0End = GBufferIDs,
		SRV0Total = SRV0End - SRV0Start + 1,
	};

	enum class RootParameterIndex {
		CbvSrvUavs,
		Count
	};

	ID3D12RootSignature* createGlobalRootSignature() {

		// CBVs
		D3D12_DESCRIPTOR_RANGE cbvRange;
		cbvRange.BaseShaderRegister = 0;
		cbvRange.NumDescriptors = UINT(DescriptorHeapConstants::CBTotal);
		cbvRange.RegisterSpace = 0;
		cbvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
		cbvRange.OffsetInDescriptorsFromTableStart = UINT(DescriptorHeapConstants::CBStart);

		// UAVs
		D3D12_DESCRIPTOR_RANGE uavRange;
		uavRange.BaseShaderRegister = 0;
		uavRange.NumDescriptors = UINT(DescriptorHeapConstants::UAV0Total);
		uavRange.RegisterSpace = 0;
		uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		uavRange.OffsetInDescriptorsFromTableStart = UINT(DescriptorHeapConstants::UAV0Start);

		// SRVs
		D3D12_DESCRIPTOR_RANGE srvRange;
		srvRange.BaseShaderRegister = 0;
		srvRange.NumDescriptors = UINT(DescriptorHeapConstants::SRV0Total);
		srvRange.RegisterSpace = 0;
		srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRange.OffsetInDescriptorsFromTableStart = UINT(DescriptorHeapConstants::SRV0Start);

		// Scene textures
		D3D12_DESCRIPTOR_RANGE sceneTexturesRange;
		sceneTexturesRange.BaseShaderRegister = 0;
		sceneTexturesRange.NumDescriptors = UINT_MAX; //< Scene texture counts can change without rebuilding pipelines.
		sceneTexturesRange.RegisterSpace = 1;
		sceneTexturesRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		sceneTexturesRange.OffsetInDescriptorsFromTableStart = UINT(DescriptorHeapConstants::SceneTexturesStart);

		D3D12_DESCRIPTOR_RANGE cbvUavSrvRanges[] = {
			cbvRange,
			uavRange,
			srvRange,
			sceneTexturesRange
		};

		// Root parameter - CBV/UAV/SRV
		D3D12_ROOT_PARAMETER paramCbvUavSrv = {};
		paramCbvUavSrv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		paramCbvUavSrv.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		paramCbvUavSrv.DescriptorTable.NumDescriptorRanges = _countof(cbvUavSrvRanges);
		paramCbvUavSrv.DescriptorTable.pDescriptorRanges = cbvUavSrvRanges;

		D3D12_ROOT_PARAMETER rootParams[UINT(RootParameterIndex::Count)];
		rootParams[UINT(RootParameterIndex::CbvSrvUavs)] = paramCbvUavSrv;

		D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
		rootDesc.NumParameters = _countof(rootParams);
		rootDesc.pParameters = rootParams;
		rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

		D3D12_STATIC_SAMPLER_DESC sceneTextureSampler = {};
		sceneTextureSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sceneTextureSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sceneTextureSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sceneTextureSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sceneTextureSampler.MaxAnisotropy = 1;
		sceneTextureSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		sceneTextureSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		sceneTextureSampler.MinLOD = 0.0f;
		sceneTextureSampler.MaxLOD = D3D12_FLOAT32_MAX;
		sceneTextureSampler.ShaderRegister = 0;
		sceneTextureSampler.RegisterSpace = 0;
		sceneTextureSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		rootDesc.NumStaticSamplers = 1;
		rootDesc.pStaticSamplers = &sceneTextureSampler;

		return createRootSignature(rootDesc);
	}

	ID3D12RootSignature* createRootSignature(const D3D12_ROOT_SIGNATURE_DESC& desc) {
		HRESULT hr;
		ID3DBlob* sig;
		ID3DBlob* error;

		hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &error);
		utils::validate(hr, L"Error: failed to serialize root signature!");

		ID3D12RootSignature* pRootSig;
		hr = mDevice->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&pRootSig));
		utils::validate(hr, L"Error: failed to create root signature!");

		SAFE_RELEASE(sig);
		SAFE_RELEASE(error);
		return pRootSig;
	}

	void uploadConstantBuffer() {

		transitionBarrier(mShaderDataCB, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);

		D3D12_SUBRESOURCE_DATA bufferDataDesc = {};
		bufferDataDesc.pData = &mShaderData;
		bufferDataDesc.RowPitch = mShaderDataCBSize;
		bufferDataDesc.SlicePitch = bufferDataDesc.RowPitch;

		UINT64 uploadedBytes = UpdateSubresources(mCmdList, mShaderDataCB, mShaderDataCBUpload[mCurrentFrameIndex], 0, 0, 1, &bufferDataDesc);
		HRESULT hr = (mShaderDataCBSize == uploadedBytes ? S_OK : E_FAIL);
		utils::validate(hr, L"Error: failed to update constant buffer!");

		transitionBarrier(mShaderDataCB, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	}

	void moveToNextFrame()
	{
		mCurrentFrameIndex = mSwapChain->GetCurrentBackBufferIndex();
		waitForGPU(mFenceValues[mCurrentFrameIndex]); // Reuse only completed frame resources.
	}

	void submitCmdList()
	{
		mCmdList->Close();

		ID3D12CommandList* pGraphicsList = { mCmdList };
		mCmdQueue->ExecuteCommandLists(1, &pGraphicsList);
		// Signal once per submission using a value shared across all frame indices.
		mFenceValues[mCurrentFrameIndex] = ++mFenceValue;
		HRESULT hr = mCmdQueue->Signal(mFence, mFenceValue);
		utils::validate(hr, L"Error: failed to signal command queue!");
	}

	void present()
	{
		// When using sync interval 0, it is recommended to always pass the tearing
		// flag when it is supported, even when presenting in windowed mode.
		// However, this flag cannot be used if the app is in fullscreen mode as a
		// result of calling SetFullscreenState.
		UINT presentFlags = (!mEnableVSync && mCapabilities.supportsTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;

		HRESULT hr = mSwapChain->Present(mEnableVSync ? 1 : 0, presentFlags);
		if (FAILED(hr))
		{
			hr = mDevice->GetDeviceRemovedReason();
			utils::validate(hr, L"Error: failed to present!");
		}
	}

	void waitForGPU(UINT64 fenceValue)
	{
		if (mFence->GetCompletedValue() >= fenceValue) return;

		HRESULT hr = mFence->SetEventOnCompletion(fenceValue, mFenceEvent);
		utils::validate(hr, L"Error: failed to set fence event!");
		WaitForSingleObjectEx(mFenceEvent, INFINITE, FALSE);
	}

	void resetCommandList()
	{
		// Reset the command allocator for the current frame
		HRESULT hr = mCmdAlloc[mCurrentFrameIndex]->Reset();
		utils::validate(hr, L"Error: failed to reset command allocator!");

		// Reset the command list for the current frame
		hr = mCmdList->Reset(mCmdAlloc[mCurrentFrameIndex], nullptr);
		utils::validate(hr, L"Error: failed to reset command list!");
	}

	D3D12_CPU_DESCRIPTOR_HANDLE getBackBufferView(UINT bufferIndex) {

		D3D12_CPU_DESCRIPTOR_HANDLE renderTargetViewHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
		renderTargetViewHandle.ptr += (mRtvDescSize * bufferIndex);

		return renderTargetViewHandle;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE getCurrentBackBufferView() {
		return getBackBufferView(mCurrentFrameIndex);
	}

	bool initImGui(HWND hwnd)
	{
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGui::StyleColorsDark();

		// Scale fonts and widget spacing once using the window's initial DPI.
		ImGuiStyle& style = ImGui::GetStyle();
		style.ScaleAllSizes(mDpiScale);
		style.FontScaleDpi = mDpiScale;

		constexpr UINT descriptorCount = 128;
		const HRESULT hr = createDescriptorHeap(mDevice, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, descriptorCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, &imguiSrvDescHeap);
		if (FAILED(hr))
		{
			return false;
		}

		// ImGui may allocate multiple texture descriptors as its font atlas grows.
		mImguiDescriptorSize = mDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		mImguiFreeDescriptors.reserve(descriptorCount);
		for (UINT i = descriptorCount; i > 0; i--) mImguiFreeDescriptors.push_back(i - 1);

		if (!ImGui_ImplWin32_Init(hwnd))
		{
			utils::validate(E_FAIL, L"Failed to initialize ImGui Win32 backend!");
			return false;
		}

		ImGui_ImplDX12_InitInfo initInfo = {};
		initInfo.Device = mDevice;
		initInfo.CommandQueue = mCmdQueue;
		initInfo.NumFramesInFlight = kMaxFramesInFlight;
		initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
		initInfo.UserData = this;
		initInfo.SrvDescriptorHeap = imguiSrvDescHeap;
		initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
		{
			auto* app = static_cast<Dx12Template*>(info->UserData);
			if (app->mImguiFreeDescriptors.empty())
			{
				utils::validate(E_FAIL, L"ImGui descriptor heap is full!");
				std::abort(); // The backend's allocation callback cannot report failure.
			}
			const UINT index = app->mImguiFreeDescriptors.back();
			app->mImguiFreeDescriptors.pop_back();
			*cpu = app->imguiSrvDescHeap->GetCPUDescriptorHandleForHeapStart();
			*gpu = app->imguiSrvDescHeap->GetGPUDescriptorHandleForHeapStart();
			cpu->ptr += SIZE_T(index) * app->mImguiDescriptorSize;
			gpu->ptr += UINT64(index) * app->mImguiDescriptorSize;
		};
		initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu)
		{
			auto* app = static_cast<Dx12Template*>(info->UserData);
			const UINT index = UINT((cpu.ptr - app->imguiSrvDescHeap->GetCPUDescriptorHandleForHeapStart().ptr) / app->mImguiDescriptorSize);
			IM_ASSERT(index < app->imguiSrvDescHeap->GetDesc().NumDescriptors);
			IM_ASSERT(gpu.ptr == app->imguiSrvDescHeap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * app->mImguiDescriptorSize);
			app->mImguiFreeDescriptors.push_back(index);
		};
		if (!ImGui_ImplDX12_Init(&initInfo))
		{
			utils::validate(E_FAIL, L"Failed to initialize ImGui DX12 backend!");
			return false;
		}
		return true;
	}

	// Dx12 Boilerplate things
	ID3D12DescriptorHeap* imguiSrvDescHeap = nullptr;
	std::wstring mAdapterName;

	ID3D12Device5* mDevice = nullptr;
	IDXGIAdapter1* mAdapter = nullptr;
	
	IDXGISwapChain3* mSwapChain = nullptr;
	ID3D12Resource* mOutputBuffer = nullptr;
	ID3D12Resource* mAccumulationBuffer = nullptr;
	ID3D12Resource* mAutoExposureBuffer = nullptr;
	TemporalAA mTAA;
	bool mEnableTAA = false;
	bool mEnableAccumulation = true;
	uint32_t mAccumulatedFrames = 0;

	DXGI_FORMAT mDepthBufferFormat = DXGI_FORMAT::DXGI_FORMAT_D32_FLOAT;
	DXGI_FORMAT mBackBufferFormat = DXGI_FORMAT::DXGI_FORMAT_R8G8B8A8_UNORM;
	ID3D12DescriptorHeap* mDsvHeap = nullptr;

	static const unsigned int kMaxFramesInFlight = 2;
	ID3D12Fence* mFence = nullptr;
	UINT64 mFenceValue = 0;
	UINT64 mFenceValues[kMaxFramesInFlight];
	HANDLE mFenceEvent;
	ID3D12Resource* mBackBuffer[kMaxFramesInFlight];

	D3D12Capabilities mCapabilities;
	bool mEnableVSync = false;

	IDXGIFactory6* mDxgiFactory = nullptr;
	float mDpiScale = 1.0f;
	UINT mImguiDescriptorSize = 0;
	std::vector<UINT> mImguiFreeDescriptors;
	unsigned int mCurrentFrameIndex = 0;
	ID3D12Resource* mDepthStencilBuffer = nullptr;

	ID3D12DescriptorHeap* mRtvHeap = nullptr;
	UINT mRtvDescSize = 0;

	ID3D12GraphicsCommandList4* mCmdList = nullptr;

	ID3D12CommandQueue* mCmdQueue = nullptr;
	ID3D12CommandAllocator* mCmdAlloc[kMaxFramesInFlight];

	struct RendererMemory
	{
		UINT64 gbuffer = 0;
		UINT64 taa = 0;
		UINT64 output = 0;
		UINT64 exposure = 0;
		UINT64 backBuffers = 0;
		UINT64 other = 0;
	} mRendererMemory;

	// Scene resources and acceleration structures
	struct SceneMemory
	{
		UINT64 geometry = 0;
		UINT64 textures = 0;
		UINT64 blas = 0;
		UINT64 tlas = 0;
		UINT64 other = 0;
	} mSceneMemory;

	ID3D12Resource* mVertexBuffer = nullptr;
	ID3D12Resource* mIndexBuffer = nullptr;
	ID3D12Resource* mInstanceDataBuffer = nullptr;
	ID3D12Resource* mEmptyBuffer = nullptr;
	ID3D12Resource* mTriangleLightsBuffer = nullptr;
	ID3D12Resource* mMaterialBuffer = nullptr;
	std::vector<ID3D12Resource*> mSceneImages;
	D3D12ResourceUploader mResourceUploader;
	std::vector<ID3D12Resource*> mBLASes;
	ID3D12Resource* mAccelerationStructureScratch = nullptr;
	ID3D12Resource* mTLAS = nullptr;
	ID3D12Resource* mTLASInstances = nullptr;
	
	ID3D12DescriptorHeap* mDescriptorHeap = nullptr;
	ID3D12RootSignature* mGlobalRootSignature = nullptr;
	UINT mCbvSrvUavDescSize = 0;
	ID3D12Debug* mDebugController = nullptr;

	ID3D12Resource* mShaderDataCB = nullptr;
	ID3D12Resource* mShaderDataCBUpload[kMaxFramesInFlight] = {};
	ShaderData mShaderData = {};
	UINT mShaderDataCBSize = 0;

	// Texture order matches the contiguous G-buffer UAV/SRV descriptor ranges.
	enum class GBufferTexture { Position, Normals, UV, MotionBarycentrics, IDs, Count };
	static constexpr DXGI_FORMAT kGBufferFormats[] = {
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R16G16B16A16_SNORM,
		DXGI_FORMAT_R32G32_FLOAT,
		DXGI_FORMAT_R32G32B32A32_FLOAT,
		DXGI_FORMAT_R32G32B32A32_UINT
	};
	ID3D12Resource* mGBufferTextures[UINT(GBufferTexture::Count)] = {};

	bool mReloadShaders = false;
	bool mShowGUI = true;
	glm::vec2 mLastMousePosition = glm::vec2(FLT_MAX, FLT_MAX);

	float mExposure = 1.0f;
	bool mEnableAutoExposure = true;
	bool mAutoExposureInitialized = false;
	bool mEnableTonemapping = true;
	bool mEnableAntiAliasing = true;
	
	unsigned int mFrameWidth = 1920;
	unsigned int mFrameHeight = 1080;
	std::vector<std::wstring> mSceneFiles;
	std::vector<std::string> mSceneNames;
	int mSelectedScene = 0;
	bool mLoadSceneRequested = false;
	std::wstring mSceneFile = L"";
	std::wstring mCameraFile = L"";

	// Renderer components
	DxcShaderCompiler mShaderCompiler;
	Profiler          mProfiler[kMaxFramesInFlight];
	Scene             mScene;
	std::vector<TriangleLight> mLights;

	// Passes
	RTPass mGBufferPass;
	RTPass mRaytracedAOPass;
	RTPass mRaytracedDIPass;
	RTPass mPathtracerPass;

	ComputePass mDebugPass;
	ComputePass mTonemappingPass;
	ComputePass mAutoExposurePass;

	enum class RendererModeType : uint {
		RaytracedAO,
		PathTracer,
		RaytracedDI,
		Debug
	};

	RendererModeType mRendererMode = RendererModeType::PathTracer;
	int mDebugView = DEBUG_VIEW_BASE_COLOR;

	float mAORadius = 1.0f;
	int mMaxBounces = 5;
	bool mEnableNEE = true;
	bool mEnableMIS = true;
	
	// Camera	
	glm::vec3 mCameraPosition = glm::vec3(0.0f, 0.0f, 0.0f);
	glm::vec3 mCameraForward = glm::vec3(-1.0f, 0.0f, 0.0f);
	glm::vec3 mCameraRight = glm::vec3(0.0f, 0.0f, 1.0f);
	glm::vec3 mCameraUp = glm::vec3(0.0f, 1.0f, 0.0f);
	float mCameraFOV = 42.0f;
	float mCameraMovementSpeed = 0.005f;
	float mCameraRotationSpeed = 0.001f;
	float mCameraMouseRotationSpeed = 0.5f;
};

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
	PAINTSTRUCT ps;
	HDC hdc;
	RECT clientRect;
	LPCREATESTRUCT pCreateStruct;

	ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
	
	Dx12Template* dx12Template = reinterpret_cast<Dx12Template*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

	switch (message) {
	case WM_CREATE:
		// Save the pointer passed in to CreateWindow as lParam.
		pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
		SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
		break;
	case WM_PAINT:
		hdc = BeginPaint(hWnd, &ps);
		EndPaint(hWnd, &ps);
		break;
	case WM_CLOSE:
		PostQuitMessage(0);
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	case WM_KEYUP:
		switch (wParam)
		{
		case VK_F1:
			if (dx12Template != nullptr) {
				dx12Template->ToggleGUI();
			}
			break;
		case VK_F5:
			if (dx12Template != nullptr) {
				dx12Template->ReloadShaders();
			}
			break;
		}
		break;
	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	return 0;
}

HRESULT Create(LONG width, LONG height, HINSTANCE& instance, HWND& window, LPCWSTR title, Dx12Template* dx12Template)
{
	// Register the window class
	WNDCLASSEX wcex = { 0 };
	wcex.cbSize = sizeof(WNDCLASSEX);
	wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WndProc;
	wcex.cbClsExtra = 0;
	wcex.cbWndExtra = 0;
	wcex.hInstance = instance;
	wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wcex.lpszMenuName = NULL;
	wcex.lpszClassName = L"Dx12Template";
	wcex.hIcon = nullptr;
	wcex.hIconSm = nullptr;

	if (!RegisterClassEx(&wcex)) {
		utils::validate(E_FAIL, L"Error: failed to register window!");
	}

	// Get the desktop resolution
	RECT desktop;
	const HWND hDesktop = GetDesktopWindow();
	GetWindowRect(hDesktop, &desktop);

	int x = (desktop.right - width) / 2;
	int y = (desktop.bottom - height) / 3;

	// Create a non-resizable window with minimize and close buttons
	const DWORD windowStyle = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

	// Create the window
	RECT rc = { 0, 0, width, height };
	AdjustWindowRect(&rc, windowStyle, FALSE);

	window = CreateWindow(wcex.lpszClassName, title, windowStyle, x, y, (rc.right - rc.left), (rc.bottom - rc.top), NULL, NULL, instance, dx12Template);
	if (!window) return E_FAIL;

	// Show the window
	ShowWindow(window, SW_SHOWDEFAULT);
	UpdateWindow(window);

	return S_OK;
}

void readCmdLineArguments(Dx12Template* dx12Template, LPWSTR lpCmdLine)
{
	// Initialize defaults
	int width = 1920;
	int height = 1080;
	const std::wstring assetsFolder = utils::getExePath() + L"assets\\";
	std::wstring sceneFile = L"";

	// Start with the Cornell box when available; command-line arguments can override it.
	const std::wstring defaultSceneFile = assetsFolder + L"cornell_box\\cornell_box.gltf";
	std::error_code error;
	if (std::filesystem::is_regular_file(defaultSceneFile, error)) sceneFile = defaultSceneFile;

	std::wstring cameraFile = L"";
	std::wstring mode;
	std::wstring debugView;
	float cameraSpeed = dx12Template->mCameraMovementSpeed * 1000.0f; // Scene units per second.

	// Create a map of known cmd. line commands
	enum class ArgDataType
	{
		INT,
		FLOAT,
		STRING,
		BOOL
	};

	std::map<std::wstring, std::tuple<void*, ArgDataType>> commands = {
			{ L"-width", { &width, ArgDataType::INT } },
			{ L"-height", { &height, ArgDataType::INT } },
			{ L"-scene", { &sceneFile, ArgDataType::STRING } },
			{ L"-camera", { &cameraFile, ArgDataType::STRING } },
			{ L"-mode", { &mode, ArgDataType::STRING } },
			{ L"-debugView", { &debugView, ArgDataType::STRING } },
			{ L"-taa", { &dx12Template->mEnableTAA, ArgDataType::BOOL } },
			{ L"-accumulate", { &dx12Template->mEnableAccumulation, ArgDataType::BOOL } },
			{ L"-aa", { &dx12Template->mEnableAntiAliasing, ArgDataType::BOOL } },
			{ L"-autoExposure", { &dx12Template->mEnableAutoExposure, ArgDataType::BOOL } },
			{ L"-exposure", { &dx12Template->mExposure, ArgDataType::FLOAT } },
			{ L"-tonemapping", { &dx12Template->mEnableTonemapping, ArgDataType::BOOL } },
			{ L"-bounces", { &dx12Template->mMaxBounces, ArgDataType::INT } },
			{ L"-nee", { &dx12Template->mEnableNEE, ArgDataType::BOOL } },
			{ L"-mis", { &dx12Template->mEnableMIS, ArgDataType::BOOL } },
			{ L"-aoRadius", { &dx12Template->mAORadius, ArgDataType::FLOAT } },
			{ L"-cameraSpeed", { &cameraSpeed, ArgDataType::FLOAT } },
			{ L"-vsync", { &dx12Template->mEnableVSync, ArgDataType::BOOL } },
			{ L"-gui", { &dx12Template->mShowGUI, ArgDataType::BOOL } }
	};

	// Read cmd. line arguments
	{
		// Split cmd line into words separated by spaces
		std::wstringstream cmdLineStream(lpCmdLine); 
		std::wstring arg;
		while (cmdLineStream >> arg) 
		{
			// Find the read argument in the map of commands
			auto cmdKey = commands.find(arg);
			if (cmdKey != commands.end())
			{
				auto& command = cmdKey->second;

				// Try to read next argument and use it as a value
				if (cmdLineStream >> arg)
				{
					ArgDataType commandTargetType = std::get<1>(command);
					if (commandTargetType == ArgDataType::INT)
					{
						int value = std::stoi(arg);
						int* targetPtr = static_cast<int*>(std::get<0>(command));
						*targetPtr = value;
                    }
                    else if (commandTargetType == ArgDataType::STRING)
                    {
                        std::wstring* targetPtr = static_cast<std::wstring*>(std::get<0>(command));
						*targetPtr = arg;
					}
					else if (commandTargetType == ArgDataType::FLOAT)
					{
						float value = std::stof(arg);
						float* targetPtr = static_cast<float*>(std::get<0>(command));
						*targetPtr = value;
					}
					else if (commandTargetType == ArgDataType::BOOL)
					{
						bool value = (arg == L"1");
						bool* targetPtr = static_cast<bool*>(std::get<0>(command));
						*targetPtr = value;
					}
				}
			}
		}
	}

	const std::map<std::wstring, Dx12Template::RendererModeType> modes = {
		{ L"ao", Dx12Template::RendererModeType::RaytracedAO }, { L"pt", Dx12Template::RendererModeType::PathTracer },
		{ L"di", Dx12Template::RendererModeType::RaytracedDI }, { L"debug", Dx12Template::RendererModeType::Debug }
	};
	const std::map<std::wstring, int> debugViews = {
		{ L"baseColor", DEBUG_VIEW_BASE_COLOR }, { L"uv", DEBUG_VIEW_UV },
		{ L"normals", DEBUG_VIEW_SHADING_NORMAL }, { L"geometryNormals", DEBUG_VIEW_GEOMETRY_NORMAL },
		{ L"instanceID", DEBUG_VIEW_INSTANCE_ID }, { L"geometryID", DEBUG_VIEW_GEOMETRY_ID },
		{ L"triangleID", DEBUG_VIEW_TRIANGLE_ID }, { L"roughness", DEBUG_VIEW_ROUGHNESS },
		{ L"metalness", DEBUG_VIEW_METALNESS }, { L"opacity", DEBUG_VIEW_OPACITY }, { L"motion", DEBUG_VIEW_MOTION_VECTORS }
	};

	// Setup the renderer
	{
		dx12Template->SetFrameSize(width, height);
		dx12Template->SetSceneFiles(sceneFile, cameraFile);
		dx12Template->mCameraMovementSpeed = cameraSpeed * 0.001f;
		if (auto it = modes.find(mode); it != modes.end()) dx12Template->mRendererMode = it->second;
		if (auto it = debugViews.find(debugView); it != debugViews.end()) dx12Template->mDebugView = it->second;
	}
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	HRESULT hr = EXIT_SUCCESS;

	{
		MSG msg = { 0 };
		HWND hWnd = { 0 };

		// Tell Windows that we're DPI aware (we handle scaling ourselves, e.g. the scaling of GUI)
		SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

		// Make an instance of the app and configure it via cmd. line args
		Dx12Template dx12Template;
		readCmdLineArguments(&dx12Template, lpCmdLine);

		int frameWidth, frameHeight;
		dx12Template.GetFrameSize(frameWidth, frameHeight);

		// Initialize window
		HRESULT hr = Create(frameWidth, frameHeight, hInstance, hWnd, L"Dx12Template", &dx12Template);
		utils::validate(hr, L"Error: failed to create window!");

		if (!dx12Template.Initialize(hWnd))
		{
			dx12Template.Cleanup();
			return EXIT_FAILURE;
		}

		std::chrono::steady_clock::time_point lastFrameTime = {};

		// Main loop
		while (WM_QUIT != msg.message)
		{
			if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}

			// Exclude synchronous scene loading from camera movement and exposure adaptation time.
			if (dx12Template.ProcessSceneChange()) lastFrameTime = std::chrono::steady_clock::now();

			// Calculate frame time
			const float elapsedTime = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - lastFrameTime).count() * 0.001f;
			lastFrameTime = std::chrono::steady_clock::now();

			dx12Template.HandleCameraMovement(hWnd, elapsedTime);

			// Break the loop here when the game is over
			if (!dx12Template.Update(hWnd, elapsedTime)) break;
		}

		dx12Template.Cleanup();
	}

	return hr;
}
