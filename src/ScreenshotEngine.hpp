// SPDX-License-Identifier: MIT
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "../include/GakuPlugin.h"
#include "../include/GakuScreenshot.h"
#include "../include/GakuMotionStageState.h"
#include "AlphaMatte.hpp"
#include "ScreenshotConfig.hpp"
#include "WicImageWriter.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ScreenshotManager
{
    struct RecentCaptureEntry
    {
        uint64_t id{0};
        std::string pathUtf8;
        std::string fileNameUtf8;
        uint32_t width{0};
        uint32_t height{0};
        uint32_t imageFormat{GAKU_IMAGE_PNG};
        bool hasAlpha{false};
        uint64_t sizeBytes{0};
        std::string timestamp;
    };

    struct CaptureCompletionNotice
    {
        uint64_t ticket{0};
        bool ok{false};
        std::string pathUtf8;
        std::string fileNameUtf8;
        uint32_t width{0};
        uint32_t height{0};
        uint32_t imageFormat{GAKU_IMAGE_PNG};
        bool hasAlpha{false};
        uint64_t sizeBytes{0};
        std::string timestamp;
        std::string errorMessage;
    };

    // Synthetic/headless render hook for offline tests without Unity/IL2CPP.
    // When set, ScreenshotEngine delegates frame capture to this callback.
    using TestRenderFn = std::function<bool(
        uint32_t width,
        uint32_t height,
        bool dualPassAlpha,
        std::vector<uint8_t>& outBlackRgba,
        std::vector<uint8_t>& outWhiteRgba,
        bool& outFlipVertical,
        std::string& outError)>;

    namespace Il2CppDetail
    {
        struct MethodType { int code; void* klass{nullptr}; bool isByRef{false}; };

        struct Il2CppApi
        {
            void* (*domainGet)(){nullptr};
            void* (*domainAssemblyOpen)(void*, const char*){nullptr};
            void* (*assemblyGetImage)(void*){nullptr};
            void* (*classFromName)(void*, const char*, const char*){nullptr};
            void* (*classGetParent)(void*){nullptr};
            void* (*classGetType)(void*){nullptr};
            void* (*typeGetObject)(void*){nullptr};
            const void* (*classGetMethods)(void*, void**){nullptr};
            const void* (*classGetMethodFromName)(void*, const char*, int){nullptr};
            const char* (*methodGetName)(const void*){nullptr};
            uint32_t (*methodGetParamCount)(const void*){nullptr};
            void* (*methodGetParam)(const void*, uint32_t){nullptr};
            void* (*methodGetReturnType)(const void*){nullptr};
            uint32_t (*methodGetFlags)(const void*, uint32_t*){nullptr};
            bool (*methodIsGeneric)(const void*){nullptr};
            bool (*methodIsInflated)(const void*){nullptr};
            void* (*methodGetDeclaringType)(const void*){nullptr};
            int (*typeGetType)(void*){nullptr};
            bool (*typeIsByref)(void*){nullptr};
            void* (*classFromType)(void*){nullptr};
            bool (*classIsValuetype)(void*){nullptr};
            int (*classValueSize)(void*, uint32_t*){nullptr};
            void* (*classGetFieldFromName)(void*, const char*){nullptr};
            void* (*fieldGetType)(void*){nullptr};
            uint32_t (*fieldGetFlags)(void*){nullptr};
            void (*fieldStaticGetValue)(void*, void*){nullptr};
            void (*fieldGetValue)(void*, void*, void*){nullptr};
            int (*fieldGetOffset)(void*){nullptr};
            void* (*runtimeInvoke)(const void*, void*, void**, void**){nullptr};
            void* (*objectUnbox)(void*){nullptr};
            void* (*objectNew)(void*){nullptr};
            void* (*objectGetClass)(void*){nullptr};
            bool (*classIsAssignableFrom)(void*, void*){nullptr};
            uintptr_t (*arrayLength)(void*){nullptr};
            void* (*gchandleNew)(void*, bool){nullptr};
            void (*gchandleFree)(void*){nullptr};
            void* (*resolveIcall)(const char*){nullptr};

            bool Resolve(HMODULE mod) noexcept
            {
                if (!mod) return false;
                auto get = [mod](auto& fn, const char* name) {
                    fn = reinterpret_cast<std::decay_t<decltype(fn)>>(GetProcAddress(mod, name));
                };
                get(domainGet, "il2cpp_domain_get");
                get(domainAssemblyOpen, "il2cpp_domain_assembly_open");
                get(assemblyGetImage, "il2cpp_assembly_get_image");
                get(classFromName, "il2cpp_class_from_name");
                get(classGetParent, "il2cpp_class_get_parent");
                get(classGetType, "il2cpp_class_get_type");
                get(typeGetObject, "il2cpp_type_get_object");
                get(classGetMethods, "il2cpp_class_get_methods");
                get(classGetMethodFromName, "il2cpp_class_get_method_from_name");
                get(methodGetName, "il2cpp_method_get_name");
                get(methodGetParamCount, "il2cpp_method_get_param_count");
                get(methodGetParam, "il2cpp_method_get_param");
                get(methodGetReturnType, "il2cpp_method_get_return_type");
                get(methodGetFlags, "il2cpp_method_get_flags");
                get(methodIsGeneric, "il2cpp_method_is_generic");
                get(methodIsInflated, "il2cpp_method_is_inflated");
                get(methodGetDeclaringType, "il2cpp_method_get_declaring_type");
                get(typeGetType, "il2cpp_type_get_type");
                get(typeIsByref, "il2cpp_type_is_byref");
                get(classFromType, "il2cpp_class_from_type");
                get(classIsValuetype, "il2cpp_class_is_valuetype");
                get(classValueSize, "il2cpp_class_value_size");
                get(classGetFieldFromName, "il2cpp_class_get_field_from_name");
                get(fieldGetType, "il2cpp_field_get_type");
                get(fieldGetFlags, "il2cpp_field_get_flags");
                get(fieldStaticGetValue, "il2cpp_field_static_get_value");
                get(fieldGetValue, "il2cpp_field_get_value");
                get(fieldGetOffset, "il2cpp_field_get_offset");
                get(runtimeInvoke, "il2cpp_runtime_invoke");
                get(objectUnbox, "il2cpp_object_unbox");
                get(objectNew, "il2cpp_object_new");
                get(objectGetClass, "il2cpp_object_get_class");
                get(classIsAssignableFrom, "il2cpp_class_is_assignable_from");
                get(arrayLength, "il2cpp_array_length");
                get(gchandleNew, "il2cpp_gchandle_new");
                get(gchandleFree, "il2cpp_gchandle_free");
                get(resolveIcall, "il2cpp_resolve_icall");

                return domainGet && domainAssemblyOpen && assemblyGetImage && classFromName &&
                    classGetMethods && methodGetName && methodGetParamCount && methodGetParam &&
                    methodGetReturnType && methodGetFlags && typeGetType && typeIsByref &&
                    classFromType && runtimeInvoke && objectUnbox;
            }

            void* FindClass(const char* assemblyName, const char* ns, const char* name) const
            {
                if (!domainGet || !domainAssemblyOpen || !assemblyGetImage || !classFromName || !assemblyName)
                    return nullptr;
                void* domain = domainGet();
                if (!domain) return nullptr;
                void* assembly = domainAssemblyOpen(domain, assemblyName);
                if (!assembly)
                {
                    const size_t len = std::strlen(assemblyName);
                    if (len > 4 && std::strcmp(assemblyName + len - 4, ".dll") == 0)
                    {
                        std::string stripped(assemblyName, len - 4);
                        assembly = domainAssemblyOpen(domain, stripped.c_str());
                    }
                }
                if (!assembly) return nullptr;
                void* image = assemblyGetImage(assembly);
                if (!image) return nullptr;
                return classFromName(image, ns, name);
            }

            bool TypeMatches(void* actual, MethodType expected) const
            {
                if (!actual || !typeIsByref || !typeGetType || !classFromType) return false;
                return typeIsByref(actual) == expected.isByRef &&
                    typeGetType(actual) == expected.code &&
                    (!expected.klass || classFromType(actual) == expected.klass);
            }

            const void* FindMethodExact(void* owner, const char* methodName, bool isStatic,
                MethodType returns, std::initializer_list<MethodType> params) const
            {
                if (!owner || !methodName || !methodIsGeneric || !methodIsInflated || !methodGetDeclaringType)
                    return nullptr;
                if ((returns.code == 0x12 || returns.code == 0x11 || returns.code == 0x1d) && !returns.klass)
                    return nullptr;
                for (const MethodType& p : params)
                    if ((p.code == 0x12 || p.code == 0x11 || p.code == 0x1d) && !p.klass)
                        return nullptr;

                const void* match = nullptr;
                void* iter = nullptr;
                while (const void* method = classGetMethods(owner, &iter))
                {
                    const char* name = methodGetName(method);
                    if (!name || std::strcmp(name, methodName) != 0) continue;
                    if (methodIsGeneric(method) || methodIsInflated(method)) continue;
                    if (methodGetDeclaringType(method) != owner) continue;
                    if (methodGetParamCount(method) != params.size()) continue;
                    uint32_t impl = 0;
                    const uint32_t attrs = methodGetFlags(method, &impl);
                    if (((attrs & 0x10u) != 0u) != isStatic || (attrs & 0x400u) != 0u) continue;
                    if (!TypeMatches(methodGetReturnType(method), returns)) continue;

                    bool ok = true;
                    uint32_t idx = 0;
                    for (const MethodType& expected : params)
                    {
                        if (!TypeMatches(methodGetParam(method, idx++), expected)) { ok = false; break; }
                    }
                    if (!ok) continue;
                    if (match) return nullptr; // Ambiguous
                    match = method;
                }
                return match;
            }

            bool ReadEnumInt(void* enumClass, const char* memberName, int& outValue) const
            {
                if (!enumClass || !classGetFieldFromName || !fieldGetFlags || !fieldStaticGetValue ||
                    !fieldGetType || !classIsValuetype || !classValueSize)
                    return false;
                uint32_t align = 0;
                if (!classIsValuetype(enumClass) || classValueSize(enumClass, &align) != sizeof(int))
                    return false;
                void* field = classGetFieldFromName(enumClass, memberName);
                if (!field || (fieldGetFlags(field) & 0x50u) != 0x50u) return false;
                if (!TypeMatches(fieldGetType(field), {0x11, enumClass})) return false;
                fieldStaticGetValue(field, &outValue);
                return true;
            }

            void* Invoke(const void* method, void* target = nullptr, void** args = nullptr) const
            {
                if (!method || !runtimeInvoke) return nullptr;
                void* ex = nullptr;
                void* res = runtimeInvoke(method, target, args, &ex);
                return ex ? nullptr : res;
            }

            bool InvokeVoid(const void* method, void* target = nullptr, void** args = nullptr) const
            {
                if (!method || !runtimeInvoke) return false;
                void* ex = nullptr;
                runtimeInvoke(method, target, args, &ex);
                return ex == nullptr;
            }

            bool ReadInt(const void* method, void* target, int& out, void** args = nullptr) const
            {
                void* boxed = Invoke(method, target, args);
                void* raw = boxed ? objectUnbox(boxed) : nullptr;
                if (!raw) return false;
                out = *static_cast<int*>(raw);
                return true;
            }

            bool ReadFloat(const void* method, void* target, float& out, void** args = nullptr) const
            {
                void* boxed = Invoke(method, target, args);
                void* raw = boxed ? objectUnbox(boxed) : nullptr;
                if (!raw) return false;
                out = *static_cast<float*>(raw);
                return true;
            }

            bool ReadBool(const void* method, void* target, bool& out, void** args = nullptr) const
            {
                void* boxed = Invoke(method, target, args);
                void* raw = boxed ? objectUnbox(boxed) : nullptr;
                if (!raw) return false;
                out = *static_cast<uint8_t*>(raw) != 0;
                return true;
            }

            bool ReadPtr(const void* method, void* target, void*& out, void** args = nullptr) const
            {
                void* boxed = Invoke(method, target, args);
                void* raw = boxed ? objectUnbox(boxed) : nullptr;
                if (!raw) return false;
                out = *static_cast<void**>(raw);
                return true;
            }
        };

        struct ScopedGcPin
        {
            const Il2CppApi* api{nullptr};
            void* handle{nullptr};
            ScopedGcPin() = default;
            ScopedGcPin(const Il2CppApi& a, void* obj) noexcept : api(&a)
            {
                if (obj && api->gchandleNew) handle = api->gchandleNew(obj, false);
            }
            ScopedGcPin(const ScopedGcPin&) = delete;
            ScopedGcPin& operator=(const ScopedGcPin&) = delete;
            ~ScopedGcPin() noexcept
            {
                if (handle && api && api->gchandleFree) api->gchandleFree(handle);
            }
        };
    } // namespace Il2CppDetail

    class ScreenshotEngine
    {
    public:
        ScreenshotEngine() = default;
        ~ScreenshotEngine() { Shutdown(); }

        void SetHostApi(const GakuHostApi* host)
        {
            std::lock_guard<std::mutex> lock(hostMutex_);
            if (host)
            {
                hostApi_ = *host;
                hostApiSet_ = true;
                host->queryService(host->hostContext, GAKU_SERVICE_LOG, GAKU_SERVICE_VERSION,
                    sizeof(logService_), &logService_);
                host->queryService(host->hostContext, GAKU_SERVICE_MAIN_THREAD, GAKU_SERVICE_VERSION,
                    sizeof(mainThreadService_), &mainThreadService_);
            }
            else
            {
                hostApi_ = {};
                hostApiSet_ = false;
                logService_ = {};
                mainThreadService_ = {};
            }
        }

        void InitializeWorker()
        {
            std::lock_guard<std::mutex> lock(workerMutex_);
            if (workerStarted_ || shuttingDown_.load(std::memory_order_acquire)) return;
            workerStop_ = false;
            workerThread_ = std::thread([this]() { WorkerLoop(); });
            workerStarted_ = true;
        }

        void Shutdown()
        {
            shuttingDown_.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                pendingQueue_.clear();
            }
            {
                std::lock_guard<std::mutex> lock(ticketMutex_);
                for (auto& [id, ticket] : tickets_)
                {
                    if (ticket && ticket->state == GAKU_CAPTURE_PENDING)
                    {
                        ticket->cancelled = true;
                        ticket->state = GAKU_CAPTURE_FAILED;
                        ticket->errorCode = GAKU_E_SHUTTING_DOWN;
                        ticket->errorMessage = "Plugin shutting down";
                    }
                }
            }
            {
                std::lock_guard<std::mutex> lock(workerMutex_);
                workerStop_ = true;
            }
            workerCv_.notify_all();
            if (workerThread_.joinable()) workerThread_.join();
            {
                std::lock_guard<std::mutex> lock(workerMutex_);
                workerStarted_ = false;
                encodeQueue_.clear();
            }
            ReleaseGpuPools();
            runtimeReady_.store(false, std::memory_order_release);
        }

        // Headless test injection helpers
        void SetTestRenderHook(TestRenderFn hook, uint32_t windowWidth = 1920u, uint32_t windowHeight = 1080u)
        {
            std::lock_guard<std::mutex> lock(testMutex_);
            testRender_ = std::move(hook);
            if (testRender_)
            {
                runtimeReady_.store(true, std::memory_order_release);
                cachedCameraAvailable_.store(true, std::memory_order_release);
                cachedWindowWidth_.store(windowWidth, std::memory_order_release);
                cachedWindowHeight_.store(windowHeight, std::memory_order_release);
            }
        }

        void SetTestGreenScreenOverride(int state) noexcept
        {
            // -1 = query service, 0 = false, 1 = true
            testGreenScreenOverride_.store(state, std::memory_order_release);
        }

        void SetTestUnityThreadBypass(bool bypass) noexcept
        {
            testMainThreadBypass_.store(bypass, std::memory_order_release);
        }

        // Called from onRuntimeReady (may be a loader worker thread - NO managed object creation).
        void OnRuntimeReady(void* gameAssembly)
        {
            if (shuttingDown_.load(std::memory_order_acquire) || !gameAssembly) return;
            std::lock_guard<std::mutex> lock(il2cppResolveMutex_);
            if (il2cppResolved_) return;
            HMODULE mod = static_cast<HMODULE>(gameAssembly);
            if (!il2cpp_.Resolve(mod))
            {
                LogWarning("[ScreenshotManager] Failed to resolve core IL2CPP exports");
                return;
            }
            ResolveUnityMetadataLocked();
            il2cppResolved_ = true;
            runtimeReady_.store(unitySymbolsReady_, std::memory_order_release);
            if (unitySymbolsReady_)
                LogInfo("[ScreenshotManager] IL2CPP runtime symbols resolved");
            else
                LogWarning("[ScreenshotManager] Unity capture symbols incomplete; capture unavailable");
        }

        // Called on Unity main thread every frame.
        void OnTick()
        {
            if (shuttingDown_.load(std::memory_order_acquire)) return;
            if (runtimeReady_.load(std::memory_order_acquire))
            {
                ResolveDeferredMainThreadTypes();
                RefreshCapabilitiesOnMainThread();
            }
            ProcessPendingFileCapturesOnMainThread();
        }

        bool IsGreenScreenActive()
        {
            const int overrideVal = testGreenScreenOverride_.load(std::memory_order_acquire);
            if (overrideVal >= 0) return overrideVal != 0;

            GakuMotionStageStateServiceV1 svc{};
            {
                std::lock_guard<std::mutex> lock(hostMutex_);
                if (!hostApiSet_ || !hostApi_.queryService) return false;
                if (!stageStateCached_)
                {
                    GakuMotionStageStateServiceV1 candidate{};
                    const GakuStatus st = hostApi_.queryService(
                        hostApi_.hostContext,
                        GAKU_SERVICE_MOTION_STAGE_STATE,
                        GAKU_MOTION_STAGE_STATE_VERSION,
                        sizeof(candidate),
                        &candidate);
                    if (st == GAKU_OK &&
                        candidate.abiVersion == GAKU_MOTION_STAGE_STATE_VERSION &&
                        candidate.structSize >= sizeof(GakuMotionStageStateServiceV1) &&
                        candidate.getState != nullptr)
                    {
                        stageStateService_ = candidate;
                        stageStateCached_ = true;
                    }
                }
                if (!stageStateCached_) return false;
                svc = stageStateService_;
            }

            GakuMotionStageStateV1 state{};
            if (svc.getState(svc.context, &state) != GAKU_OK) return false;
            if (state.abiVersion != GAKU_MOTION_STAGE_STATE_VERSION ||
                state.structSize < sizeof(GakuMotionStageStateV1))
                return false;
            return state.greenScreenActive != 0;
        }

        // ---- GakuScreenshotServiceV1 implementation --------------------------

        GakuStatus GetCapabilities(GakuScreenshotCapabilitiesV1* output)
        {
            if (!output) return GAKU_E_INVALID_ARGUMENT;
            if (output->structSize != 0 && output->structSize < sizeof(GakuScreenshotCapabilitiesV1))
                return GAKU_E_SIZE;

            if (IsMainThread() && runtimeReady_.load(std::memory_order_acquire))
            {
                ResolveDeferredMainThreadTypes();
                RefreshCapabilitiesOnMainThread();
            }

            GakuScreenshotCapabilitiesV1 caps{};
            caps.abiVersion = GAKU_SCREENSHOT_VERSION;
            caps.structSize = sizeof(GakuScreenshotCapabilitiesV1);
            caps.runtimeReady = runtimeReady_.load(std::memory_order_acquire) ? 1u : 0u;
            caps.cameraAvailable = (caps.runtimeReady && cachedCameraAvailable_.load(std::memory_order_acquire)) ? 1u : 0u;
            caps.greenScreenDetected = IsGreenScreenActive() ? 1u : 0u;
            caps.maxDimension = kMaxCaptureDimension;
            caps.windowWidth = cachedWindowWidth_.load(std::memory_order_acquire);
            caps.windowHeight = cachedWindowHeight_.load(std::memory_order_acquire);
            *output = caps;
            return GAKU_OK;
        }

        GakuStatus RequestFileCapture(const GakuScreenshotFileRequestV1* request, uint64_t* outTicket,
            const std::filesystem::path& fallbackOutputDir = {})
        {
            if (!request || !outTicket) return GAKU_E_INVALID_ARGUMENT;
            *outTicket = 0;
            if (shuttingDown_.load(std::memory_order_acquire)) return GAKU_E_SHUTTING_DOWN;
            if (request->abiVersion != GAKU_SCREENSHOT_VERSION) return GAKU_E_VERSION;
            if (request->structSize < sizeof(GakuScreenshotFileRequestV1)) return GAKU_E_SIZE;

            if (!IsValidResolution(request->width, request->height, kMaxCaptureDimension))
                return GAKU_E_INVALID_ARGUMENT;
            if (request->imageFormat != GAKU_IMAGE_PNG && request->imageFormat != GAKU_IMAGE_JPEG)
                return GAKU_E_INVALID_ARGUMENT;
            if (request->alphaMode > GAKU_SCREENSHOT_ALPHA_OPAQUE)
                return GAKU_E_INVALID_ARGUMENT;
            if (request->jpegQuality > 100u)
                return GAKU_E_INVALID_ARGUMENT;

            // Validate null-terminated path string in fixed buffer
            const size_t pathLen = strnlen(request->outputPathUtf8, sizeof(request->outputPathUtf8));
            if (pathLen >= sizeof(request->outputPathUtf8))
                return GAKU_E_INVALID_ARGUMENT;

            std::filesystem::path targetPath;
            if (pathLen > 0)
            {
                std::wstring widePath;
                if (!Utf8ToWide(std::string_view(request->outputPathUtf8, pathLen), widePath))
                    return GAKU_E_INVALID_ARGUMENT;
                targetPath = std::filesystem::path(widePath);
            }
            else if (!fallbackOutputDir.empty())
            {
                targetPath = GenerateScreenshotFilePath(fallbackOutputDir, request->imageFormat);
            }
            else
            {
                return GAKU_E_INVALID_ARGUMENT;
            }

            if (!runtimeReady_.load(std::memory_order_acquire))
                return GAKU_E_UNAVAILABLE;

            InitializeWorker();

            std::string savedPathUtf8 = PathToUtf8(targetPath);
            if (savedPathUtf8.empty() || savedPathUtf8.size() >= sizeof(request->outputPathUtf8))
                return GAKU_E_INVALID_ARGUMENT;

            auto ticketObj = std::make_shared<TicketState>();
            {
                std::lock_guard<std::mutex> lock(ticketMutex_);
                if (tickets_.size() >= 64u) return GAKU_E_LIMIT;
                const uint64_t id = nextTicketId_.fetch_add(1, std::memory_order_relaxed);
                ticketObj->id = id;
                ticketObj->state = GAKU_CAPTURE_PENDING;
                ticketObj->savedPath = targetPath;
                ticketObj->savedPathUtf8 = savedPathUtf8;
                ticketObj->requestedWidth = request->width;
                ticketObj->requestedHeight = request->height;
                ticketObj->imageFormat = request->imageFormat;
                ticketObj->jpegQuality = request->jpegQuality == 0u ? kDefaultJpegQuality : request->jpegQuality;
                ticketObj->alphaMode = request->alphaMode;
                tickets_.emplace(id, ticketObj);
                *outTicket = id;
            }

            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                pendingQueue_.push_back(ticketObj);
            }

            if (IsMainThread())
            {
                ProcessPendingFileCapturesOnMainThread();
            }
            else
            {
                ScheduleMainThreadDrain();
            }
            return GAKU_OK;
        }

        GakuStatus PollCapture(uint64_t ticket, GakuScreenshotResultV1* output)
        {
            if (!ticket || !output) return GAKU_E_INVALID_ARGUMENT;
            if (output->structSize != 0 && output->structSize < sizeof(GakuScreenshotResultV1))
                return GAKU_E_SIZE;

            std::lock_guard<std::mutex> lock(ticketMutex_);
            const auto it = tickets_.find(ticket);
            if (it == tickets_.end() || !it->second) return GAKU_E_INVALID_ARGUMENT;

            const TicketState& t = *it->second;
            GakuScreenshotResultV1 res{};
            res.abiVersion = GAKU_SCREENSHOT_VERSION;
            res.structSize = sizeof(GakuScreenshotResultV1);
            res.state = t.state;
            res.width = t.width;
            res.height = t.height;
            res.hasAlpha = t.hasAlpha ? 1u : 0u;
            res.errorCode = t.errorCode;
            CopyCString(res.savedPathUtf8, sizeof(res.savedPathUtf8), t.savedPathUtf8);
            CopyCString(res.errorMessage, sizeof(res.errorMessage), t.errorMessage);
            *output = res;
            return GAKU_OK;
        }

        GakuStatus ReleaseTicket(uint64_t ticket)
        {
            if (!ticket) return GAKU_E_INVALID_ARGUMENT;
            std::lock_guard<std::mutex> lock(ticketMutex_);
            const auto it = tickets_.find(ticket);
            if (it == tickets_.end()) return GAKU_E_INVALID_ARGUMENT;
            if (it->second) it->second->cancelled = true;
            tickets_.erase(it);
            return GAKU_OK;
        }

        GakuStatus CaptureFrameImmediate(const GakuScreenshotFrameRequestV1* request,
            void* outPixels, size_t capacity, GakuScreenshotFrameInfoV1* outInfo,
            char* errorBuffer, uint32_t errorCapacity)
        {
            auto setErr = [&](const std::string& msg) {
                if (errorBuffer && errorCapacity > 0)
                    CopyCString(errorBuffer, errorCapacity, msg);
            };

            if (!request || !outInfo)
            {
                setErr("Invalid request or outInfo argument");
                return GAKU_E_INVALID_ARGUMENT;
            }
            if (outInfo->structSize != 0 && outInfo->structSize < sizeof(GakuScreenshotFrameInfoV1))
            {
                setErr("outInfo structSize is too small");
                return GAKU_E_SIZE;
            }
            if (request->abiVersion != GAKU_SCREENSHOT_VERSION)
            {
                setErr("Unsupported GakuScreenshotFrameRequestV1 abiVersion");
                return GAKU_E_VERSION;
            }
            if (request->structSize < sizeof(GakuScreenshotFrameRequestV1))
            {
                setErr("Invalid GakuScreenshotFrameRequestV1 structSize");
                return GAKU_E_SIZE;
            }
            if (!IsValidResolution(request->width, request->height, kMaxCaptureDimension))
            {
                setErr("Invalid capture resolution");
                return GAKU_E_INVALID_ARGUMENT;
            }
            if (request->pixelFormat != GAKU_PIXEL_RGBA8_SRGB_TOPDOWN &&
                request->pixelFormat != GAKU_PIXEL_BGRA8_SRGB_TOPDOWN)
            {
                setErr("Unsupported pixel format");
                return GAKU_E_INVALID_ARGUMENT;
            }
            if (request->alphaMode > GAKU_SCREENSHOT_ALPHA_OPAQUE)
            {
                setErr("Invalid alpha mode");
                return GAKU_E_INVALID_ARGUMENT;
            }
            if (shuttingDown_.load(std::memory_order_acquire))
            {
                setErr("ScreenshotManager is shutting down");
                return GAKU_E_SHUTTING_DOWN;
            }
            if (!runtimeReady_.load(std::memory_order_acquire))
            {
                setErr("Runtime is not ready");
                return GAKU_E_UNAVAILABLE;
            }
            if (!IsMainThread())
            {
                setErr("captureFrameImmediate must be called on the Unity main thread");
                return GAKU_E_UNAVAILABLE;
            }

            uint32_t targetW = request->width;
            uint32_t targetH = request->height;
            if (!ResolveTargetDimensions(targetW, targetH))
            {
                setErr("Cannot resolve target resolution");
                return GAKU_E_UNAVAILABLE;
            }

            const size_t pitch = static_cast<size_t>(targetW) * 4u;
            const size_t needed = pitch * static_cast<size_t>(targetH);
            if (!outPixels || capacity < needed)
            {
                GakuScreenshotFrameInfoV1 info{};
                info.abiVersion = GAKU_SCREENSHOT_VERSION;
                info.structSize = sizeof(GakuScreenshotFrameInfoV1);
                info.width = targetW;
                info.height = targetH;
                info.rowPitch = static_cast<uint32_t>(pitch);
                info.hasAlpha = 0u;
                info.unityFrame = ReadUnityFrameCount();
                *outInfo = info;
                setErr("Output pixel buffer capacity is too small");
                return GAKU_E_SIZE;
            }

            const bool useAlpha = ShouldCaptureAlpha(request->alphaMode);
            bool flipVertical = false;
            std::string captureError;
            if (!CaptureRawBuffers(targetW, targetH, useAlpha,
                immediateBlack_, immediateWhite_, flipVertical, captureError))
            {
                setErr(captureError);
                return GAKU_E_UNAVAILABLE;
            }

            uint8_t* dst = static_cast<uint8_t*>(outPixels);
            bool matteOk = false;
            if (useAlpha)
            {
                matteOk = AlphaMatte::ExtractAlphaDifferenceMatte(
                    targetW, targetH, immediateBlack_.data(), immediateWhite_.data(), dst, flipVertical);
            }
            else
            {
                matteOk = AlphaMatte::CopyOpaqueToRgba(
                    targetW, targetH, immediateBlack_.data(), flipVertical, dst);
            }

            if (!matteOk)
            {
                setErr("Alpha/pixel conversion failed");
                return GAKU_E_INTERNAL;
            }

            if (request->pixelFormat == GAKU_PIXEL_BGRA8_SRGB_TOPDOWN)
            {
                AlphaMatte::RgbaToBgra(targetW, targetH, dst, dst);
            }

            GakuScreenshotFrameInfoV1 info{};
            info.abiVersion = GAKU_SCREENSHOT_VERSION;
            info.structSize = sizeof(GakuScreenshotFrameInfoV1);
            info.width = targetW;
            info.height = targetH;
            info.rowPitch = static_cast<uint32_t>(pitch);
            info.hasAlpha = useAlpha ? 1u : 0u;
            info.unityFrame = ReadUnityFrameCount();
            *outInfo = info;
            return GAKU_OK;
        }

        // ---- Recent captures & UI status inspection --------------------------

        void CopyStatusSnapshot(bool& outInFlight, uint32_t& outPendingCount, std::string& outLastError,
            std::vector<RecentCaptureEntry>& outRecent, std::vector<CaptureCompletionNotice>& outCompletions)
        {
            {
                std::lock_guard<std::mutex> lock(recentMutex_);
                outRecent = recentCaptures_;
                outLastError = lastCaptureError_;
                outCompletions.assign(
                    std::make_move_iterator(completionQueue_.begin()),
                    std::make_move_iterator(completionQueue_.end()));
                completionQueue_.clear();
            }
            size_t queuedMain = 0;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                queuedMain = pendingQueue_.size();
            }
            size_t queuedEncode = 0;
            {
                std::lock_guard<std::mutex> lock(workerMutex_);
                queuedEncode = encodeQueue_.size();
            }
            const uint32_t active = activeCount_.load(std::memory_order_acquire);
            outPendingCount = static_cast<uint32_t>(queuedMain + queuedEncode + active);
            outInFlight = outPendingCount > 0;
        }

    private:
        struct TicketState
        {
            uint64_t id{0};
            uint32_t state{GAKU_CAPTURE_PENDING};
            uint32_t requestedWidth{0};
            uint32_t requestedHeight{0};
            uint32_t width{0};
            uint32_t height{0};
            uint32_t imageFormat{GAKU_IMAGE_PNG};
            uint32_t jpegQuality{kDefaultJpegQuality};
            uint32_t alphaMode{GAKU_SCREENSHOT_ALPHA_AUTO_GREENSCREEN};
            bool hasAlpha{false};
            int32_t errorCode{0};
            std::filesystem::path savedPath;
            std::string savedPathUtf8;
            std::string errorMessage;
            std::atomic<bool> cancelled{false};
        };

        struct EncodeJob
        {
            std::shared_ptr<TicketState> ticket;
            uint32_t width{0};
            uint32_t height{0};
            bool useAlpha{false};
            bool flipVertical{false};
            std::vector<uint8_t> blackRgba;
            std::vector<uint8_t> whiteRgba;
        };

        static void CopyCString(char* dst, size_t capacity, const std::string& src) noexcept
        {
            if (!dst || capacity == 0) return;
            const size_t n = (std::min)(src.size(), capacity - 1u);
            if (n > 0) std::memcpy(dst, src.data(), n);
            dst[n] = '\0';
        }

        void LogInfo(const char* msg)
        {
            std::lock_guard<std::mutex> lock(hostMutex_);
            if (logService_.write) logService_.write(logService_.context, GAKU_LOG_INFO, msg);
        }

        void LogWarning(const char* msg)
        {
            std::lock_guard<std::mutex> lock(hostMutex_);
            if (logService_.write) logService_.write(logService_.context, GAKU_LOG_WARNING, msg);
        }

        bool IsMainThread() const
        {
            if (testMainThreadBypass_.load(std::memory_order_acquire)) return true;
            std::lock_guard<std::mutex> lock(hostMutex_);
            if (mainThreadService_.isMainThread)
                return mainThreadService_.isMainThread(mainThreadService_.context) != 0;
            return false;
        }

        static void MainThreadDrainThunk(void* userData)
        {
            if (auto* self = static_cast<ScreenshotEngine*>(userData))
            {
                self->ProcessPendingFileCapturesOnMainThread();
            }
        }

        void ScheduleMainThreadDrain()
        {
            std::lock_guard<std::mutex> lock(hostMutex_);
            if (mainThreadService_.enqueue)
            {
                mainThreadService_.enqueue(mainThreadService_.context, &ScreenshotEngine::MainThreadDrainThunk, this);
            }
        }

        bool ShouldCaptureAlpha(uint32_t alphaMode)
        {
            if (alphaMode == GAKU_SCREENSHOT_ALPHA_OPAQUE) return false;
            if (alphaMode == GAKU_SCREENSHOT_ALPHA_FORCE_TRANSPARENT) return true;
            return IsGreenScreenActive();
        }

        bool ResolveTargetDimensions(uint32_t& width, uint32_t& height)
        {
            if (width > 0 && height > 0)
                return width <= kMaxCaptureDimension && height <= kMaxCaptureDimension;

            uint32_t w = 0, h = 0;
            {
                std::lock_guard<std::mutex> lock(testMutex_);
                if (testRender_)
                {
                    w = cachedWindowWidth_.load(std::memory_order_acquire);
                    h = cachedWindowHeight_.load(std::memory_order_acquire);
                }
            }
            if ((w == 0 || h == 0) && unitySymbolsReady_)
            {
                ReadScreenDimensions(w, h);
            }
            if (w == 0 || h == 0) return false;
            width = (std::min)(w, kMaxCaptureDimension);
            height = (std::min)(h, kMaxCaptureDimension);
            return true;
        }

        // ---- IL2CPP resolution & Unity capture -------------------------------

        void ResolveUnityMetadataLocked()
        {
            const auto core = [&](const char* ns, const char* name) {
                return il2cpp_.FindClass("UnityEngine.CoreModule.dll", ns, name);
            };

            objectClass_ = core("UnityEngine", "Object");
            cameraClass_ = core("UnityEngine", "Camera");
            componentClass_ = core("UnityEngine", "Component");
            gameObjectClass_ = core("UnityEngine", "GameObject");
            renderTextureClass_ = core("UnityEngine", "RenderTexture");
            textureClass_ = core("UnityEngine", "Texture");
            texture2DClass_ = core("UnityEngine", "Texture2D");
            screenClass_ = core("UnityEngine", "Screen");
            timeClass_ = core("UnityEngine", "Time");
            colorClass_ = core("UnityEngine", "Color");
            rectClass_ = core("UnityEngine", "Rect");
            clearFlagsClass_ = core("UnityEngine", "CameraClearFlags");
            rtFormatClass_ = core("UnityEngine", "RenderTextureFormat");
            rtReadWriteClass_ = core("UnityEngine", "RenderTextureReadWrite");
            texFormatClass_ = core("UnityEngine", "TextureFormat");
            graphicsSettingsClass_ = core("UnityEngine.Rendering", "GraphicsSettings");
            renderPipelineAssetClass_ = core("UnityEngine.Rendering", "RenderPipelineAsset");

            void* mscorlib = il2cpp_.FindClass("mscorlib.dll", "System", "Type");
            typeClass_ = mscorlib;
            void* byteClass = il2cpp_.FindClass("mscorlib.dll", "System", "Byte");
            byteArrayClass_ = il2cpp_.FindClass("mscorlib.dll", "System", "Byte[]");

            // Validate value-type ABI sizes (Color = 4 floats, Rect = 4 floats)
            uint32_t align = 0;
            const bool colorOk = colorClass_ && il2cpp_.classIsValuetype && il2cpp_.classValueSize &&
                il2cpp_.classIsValuetype(colorClass_) && il2cpp_.classValueSize(colorClass_, &align) == 16;
            const bool rectOk = rectClass_ && il2cpp_.classIsValuetype && il2cpp_.classValueSize &&
                il2cpp_.classIsValuetype(rectClass_) && il2cpp_.classValueSize(rectClass_, &align) == 16;

            if (!objectClass_ || !cameraClass_ || !renderTextureClass_ || !textureClass_ || !colorOk)
            {
                unitySymbolsReady_ = false;
                return;
            }

            // Enums (fallback to Unity documented integer values if stripped)
            int val = 0;
            if (il2cpp_.ReadEnumInt(clearFlagsClass_, "SolidColor", val)) enumClearSolidColor_ = val;
            if (il2cpp_.ReadEnumInt(rtFormatClass_, "ARGB32", val)) enumRtArgb32_ = val;
            if (il2cpp_.ReadEnumInt(rtReadWriteClass_, "sRGB", val)) enumRtSrgb_ = val;
            if (il2cpp_.ReadEnumInt(texFormatClass_, "RGBA32", val)) enumTexRgba32_ = val;

            // Object
            mObjectAlive_ = il2cpp_.FindMethodExact(objectClass_, "op_Implicit", true, {0x02}, {{0x12, objectClass_}});
            mObjectDestroyImmediate_ = il2cpp_.FindMethodExact(objectClass_, "DestroyImmediate", true, {0x01}, {{0x12, objectClass_}});
            if (!mObjectDestroyImmediate_)
                mObjectDestroyImmediate_ = il2cpp_.FindMethodExact(objectClass_, "Destroy", true, {0x01}, {{0x12, objectClass_}});

            // Camera
            mCameraGetMain_ = il2cpp_.FindMethodExact(cameraClass_, "get_main", true, {0x12, cameraClass_}, {});
            mCameraGetTargetTex_ = il2cpp_.FindMethodExact(cameraClass_, "get_targetTexture", false, {0x12, renderTextureClass_}, {});
            mCameraSetTargetTex_ = il2cpp_.FindMethodExact(cameraClass_, "set_targetTexture", false, {0x01}, {{0x12, renderTextureClass_}});
            mCameraGetAspect_ = il2cpp_.FindMethodExact(cameraClass_, "get_aspect", false, {0x0c}, {});
            mCameraSetAspect_ = il2cpp_.FindMethodExact(cameraClass_, "set_aspect", false, {0x01}, {{0x0c}});
            mCameraResetAspect_ = il2cpp_.FindMethodExact(cameraClass_, "ResetAspect", false, {0x01}, {});
            mCameraGetClearFlags_ = il2cpp_.FindMethodExact(cameraClass_, "get_clearFlags", false, {0x11, clearFlagsClass_}, {});
            mCameraSetClearFlags_ = il2cpp_.FindMethodExact(cameraClass_, "set_clearFlags", false, {0x01}, {{0x11, clearFlagsClass_}});
            mCameraGetBgColor_ = il2cpp_.FindMethodExact(cameraClass_, "get_backgroundColor", false, {0x11, colorClass_}, {});
            mCameraSetBgColor_ = il2cpp_.FindMethodExact(cameraClass_, "set_backgroundColor", false, {0x01}, {{0x11, colorClass_}});
            mCameraRender_ = il2cpp_.FindMethodExact(cameraClass_, "Render", false, {0x01}, {});
            mCameraGetPixelWidth_ = il2cpp_.FindMethodExact(cameraClass_, "get_pixelWidth", false, {0x08}, {});
            mCameraGetPixelHeight_ = il2cpp_.FindMethodExact(cameraClass_, "get_pixelHeight", false, {0x08}, {});

            // Fallback icall lookup for get/set_backgroundColor_Injected if managed wrapper missing
            if (!mCameraGetBgColor_ || !mCameraSetBgColor_)
            {
                ResolveCachedPtrFallbackLocked();
            }

            // RenderTexture
            mRtGetTemporary5_ = rtFormatClass_ && rtReadWriteClass_
                ? il2cpp_.FindMethodExact(renderTextureClass_, "GetTemporary", true, {0x12, renderTextureClass_},
                    {{0x08}, {0x08}, {0x08}, {0x11, rtFormatClass_}, {0x11, rtReadWriteClass_}})
                : nullptr;
            mRtGetTemporary4_ = rtFormatClass_
                ? il2cpp_.FindMethodExact(renderTextureClass_, "GetTemporary", true, {0x12, renderTextureClass_},
                    {{0x08}, {0x08}, {0x08}, {0x11, rtFormatClass_}})
                : nullptr;
            mRtGetTemporary3_ = il2cpp_.FindMethodExact(renderTextureClass_, "GetTemporary", true, {0x12, renderTextureClass_},
                {{0x08}, {0x08}, {0x08}});
            mRtGetTemporary2_ = il2cpp_.FindMethodExact(renderTextureClass_, "GetTemporary", true, {0x12, renderTextureClass_},
                {{0x08}, {0x08}});
            mRtReleaseTemporary_ = il2cpp_.FindMethodExact(renderTextureClass_, "ReleaseTemporary", true, {0x01}, {{0x12, renderTextureClass_}});
            mRtGetActive_ = il2cpp_.FindMethodExact(renderTextureClass_, "get_active", true, {0x12, renderTextureClass_}, {});
            mRtSetActive_ = il2cpp_.FindMethodExact(renderTextureClass_, "set_active", true, {0x01}, {{0x12, renderTextureClass_}});
            mRtCreate_ = il2cpp_.FindMethodExact(renderTextureClass_, "Create", false, {0x02}, {});

            // Texture::GetNativeTexturePtr
            mTexGetNativePtr_ = il2cpp_.FindMethodExact(textureClass_, "GetNativeTexturePtr", false, {0x18}, {});

            // Texture2D fallback
            if (texture2DClass_ && rectOk)
            {
                if (texFormatClass_)
                    mTex2DCtor4_ = il2cpp_.FindMethodExact(texture2DClass_, ".ctor", false, {0x01},
                        {{0x08}, {0x08}, {0x11, texFormatClass_}, {0x02}});
                mTex2DCtor2_ = il2cpp_.FindMethodExact(texture2DClass_, ".ctor", false, {0x01}, {{0x08}, {0x08}});
                mTex2DReadPixels4_ = il2cpp_.FindMethodExact(texture2DClass_, "ReadPixels", false, {0x01},
                    {{0x11, rectClass_}, {0x08}, {0x08}, {0x02}});
                mTex2DReadPixels3_ = il2cpp_.FindMethodExact(texture2DClass_, "ReadPixels", false, {0x01},
                    {{0x11, rectClass_}, {0x08}, {0x08}});
                if (byteArrayClass_)
                    mTex2DGetRawBytes_ = il2cpp_.FindMethodExact(texture2DClass_, "GetRawTextureData", false, {0x1d, byteArrayClass_}, {});
                if (!mTex2DGetRawBytes_ && il2cpp_.classGetMethodFromName)
                {
                    // Fallback by name+arity if Byte[] class resolution differed
                    void* iter = nullptr;
                    while (const void* m = il2cpp_.classGetMethods(texture2DClass_, &iter))
                    {
                        const char* n = il2cpp_.methodGetName(m);
                        if (n && std::strcmp(n, "GetRawTextureData") == 0 &&
                            !il2cpp_.methodIsGeneric(m) && il2cpp_.methodGetParamCount(m) == 0 &&
                            il2cpp_.methodGetDeclaringType(m) == texture2DClass_)
                        {
                            void* ret = il2cpp_.methodGetReturnType(m);
                            if (ret && il2cpp_.typeGetType(ret) == 0x1d)
                            {
                                mTex2DGetRawBytes_ = m;
                                break;
                            }
                        }
                    }
                }
            }
            (void)byteClass;

            // Screen / Time
            if (screenClass_)
            {
                mScreenGetWidth_ = il2cpp_.FindMethodExact(screenClass_, "get_width", true, {0x08}, {});
                mScreenGetHeight_ = il2cpp_.FindMethodExact(screenClass_, "get_height", true, {0x08}, {});
            }
            if (timeClass_)
            {
                mTimeGetFrameCount_ = il2cpp_.FindMethodExact(timeClass_, "get_frameCount", true, {0x08}, {});
            }

            // URP renderScale (optional)
            if (graphicsSettingsClass_ && renderPipelineAssetClass_)
            {
                mGetCurrentPipeline_ = il2cpp_.FindMethodExact(graphicsSettingsClass_, "get_currentRenderPipeline",
                    true, {0x12, renderPipelineAssetClass_}, {});
            }
            urpAssetClass_ = il2cpp_.FindClass("Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Rendering.Universal", "UniversalRenderPipelineAsset");
            if (urpAssetClass_)
            {
                mUrpGetRenderScale_ = il2cpp_.FindMethodExact(urpAssetClass_, "get_renderScale", false, {0x0c}, {});
                mUrpSetRenderScale_ = il2cpp_.FindMethodExact(urpAssetClass_, "set_renderScale", false, {0x01}, {{0x0c}});
            }
            urpAdditionalCamClass_ = il2cpp_.FindClass("Unity.RenderPipelines.Universal.Runtime.dll",
                "UnityEngine.Rendering.Universal", "UniversalAdditionalCameraData");
            if (urpAdditionalCamClass_ && componentClass_ && gameObjectClass_ && typeClass_)
            {
                mCompGetGameObject_ = il2cpp_.FindMethodExact(componentClass_, "get_gameObject", false, {0x12, gameObjectClass_}, {});
                mGoGetComponent_ = il2cpp_.FindMethodExact(gameObjectClass_, "GetComponent", false, {0x12, componentClass_}, {{0x12, typeClass_}});
                mCamDataGetRenderScale_ = il2cpp_.FindMethodExact(urpAdditionalCamClass_, "get_renderScale", false, {0x0c}, {});
                mCamDataSetRenderScale_ = il2cpp_.FindMethodExact(urpAdditionalCamClass_, "set_renderScale", false, {0x01}, {{0x0c}});
                if (mCamDataGetRenderScale_ && mCamDataSetRenderScale_ && il2cpp_.classGetType && il2cpp_.typeGetObject)
                    pendingCamDataType_.store(true, std::memory_order_release);
            }

            const bool hasRtAlloc = (mRtGetTemporary5_ || mRtGetTemporary4_ || mRtGetTemporary3_ || mRtGetTemporary2_) &&
                mRtReleaseTemporary_;
            const bool hasReadback = (mTexGetNativePtr_ != nullptr) ||
                ((mTex2DCtor4_ || mTex2DCtor2_) && (mTex2DReadPixels4_ || mTex2DReadPixels3_) &&
                 mTex2DGetRawBytes_ && mRtGetActive_ && mRtSetActive_ && il2cpp_.objectNew && il2cpp_.arrayLength);
            const bool hasBgAccess = (mCameraGetBgColor_ && mCameraSetBgColor_) ||
                (fnCameraGetBgInjected_ && fnCameraSetBgInjected_ && cachedPtrField_);

            unitySymbolsReady_ = mObjectAlive_ && mCameraGetMain_ && mCameraGetTargetTex_ && mCameraSetTargetTex_ &&
                mCameraGetAspect_ && mCameraSetAspect_ && mCameraGetClearFlags_ && mCameraSetClearFlags_ &&
                hasBgAccess && mCameraRender_ && hasRtAlloc && hasReadback;
        }

        void ResolveCachedPtrFallbackLocked()
        {
            if (!objectClass_ || !cameraClass_ || !il2cpp_.classGetFieldFromName || !il2cpp_.fieldGetValue ||
                !il2cpp_.resolveIcall || !il2cpp_.classGetMethodFromName)
                return;
            cachedPtrField_ = il2cpp_.classGetFieldFromName(objectClass_, "m_CachedPtr");
            const bool nativeSelf = il2cpp_.classGetMethodFromName(cameraClass_, "get_fieldOfView_Injected", 1) != nullptr;
            if (nativeSelf)
            {
                fnCameraGetBgInjected_ = reinterpret_cast<void (*)(void*, float*)>(
                    il2cpp_.resolveIcall("UnityEngine.Camera::get_backgroundColor_Injected(System.IntPtr,UnityEngine.Color&)"));
                fnCameraSetBgInjected_ = reinterpret_cast<void (*)(void*, const float*)>(
                    il2cpp_.resolveIcall("UnityEngine.Camera::set_backgroundColor_Injected(System.IntPtr,UnityEngine.Color&)"));
            }
            else
            {
                fnCameraGetBgInjected_ = reinterpret_cast<void (*)(void*, float*)>(
                    il2cpp_.resolveIcall("UnityEngine.Camera::get_backgroundColor_Injected(UnityEngine.Color&)"));
                fnCameraSetBgInjected_ = reinterpret_cast<void (*)(void*, const float*)>(
                    il2cpp_.resolveIcall("UnityEngine.Camera::set_backgroundColor_Injected(UnityEngine.Color&)"));
            }
        }

        void ResolveDeferredMainThreadTypes()
        {
            if (!pendingCamDataType_.load(std::memory_order_acquire)) return;
            if (!urpAdditionalCamClass_ || !il2cpp_.classGetType || !il2cpp_.typeGetObject)
            {
                pendingCamDataType_.store(false, std::memory_order_release);
                return;
            }
            if (void* typePtr = il2cpp_.classGetType(urpAdditionalCamClass_))
            {
                urpAdditionalCamTypeObj_ = il2cpp_.typeGetObject(typePtr);
            }
            pendingCamDataType_.store(false, std::memory_order_release);
        }

        bool IsUnityAlive(void* obj) const
        {
            if (!obj || !mObjectAlive_) return false;
            bool alive = false;
            void* args[1]{obj};
            return il2cpp_.ReadBool(mObjectAlive_, nullptr, alive, args) && alive;
        }

        void ReadScreenDimensions(uint32_t& outW, uint32_t& outH) const
        {
            outW = 0;
            outH = 0;
            int w = 0, h = 0;
            if (mScreenGetWidth_ && mScreenGetHeight_ &&
                il2cpp_.ReadInt(mScreenGetWidth_, nullptr, w) &&
                il2cpp_.ReadInt(mScreenGetHeight_, nullptr, h) &&
                w > 0 && h > 0)
            {
                outW = static_cast<uint32_t>(w);
                outH = static_cast<uint32_t>(h);
                return;
            }
            void* cam = mCameraGetMain_ ? il2cpp_.Invoke(mCameraGetMain_) : nullptr;
            if (cam && IsUnityAlive(cam) && mCameraGetPixelWidth_ && mCameraGetPixelHeight_ &&
                il2cpp_.ReadInt(mCameraGetPixelWidth_, cam, w) &&
                il2cpp_.ReadInt(mCameraGetPixelHeight_, cam, h) &&
                w > 0 && h > 0)
            {
                outW = static_cast<uint32_t>(w);
                outH = static_cast<uint32_t>(h);
            }
        }

        uint64_t ReadUnityFrameCount() const
        {
            if (!unitySymbolsReady_ || !mTimeGetFrameCount_) return 0;
            int frame = 0;
            return il2cpp_.ReadInt(mTimeGetFrameCount_, nullptr, frame) && frame >= 0
                ? static_cast<uint64_t>(frame)
                : 0ull;
        }

        void RefreshCapabilitiesOnMainThread()
        {
            if (!unitySymbolsReady_) return;
            void* cam = mCameraGetMain_ ? il2cpp_.Invoke(mCameraGetMain_) : nullptr;
            cachedCameraAvailable_.store(cam && IsUnityAlive(cam), std::memory_order_release);
            uint32_t w = 0, h = 0;
            ReadScreenDimensions(w, h);
            if (w > 0 && h > 0)
            {
                cachedWindowWidth_.store(w, std::memory_order_release);
                cachedWindowHeight_.store(h, std::memory_order_release);
            }
        }

        struct UnityColor { float r{0}, g{0}, b{0}, a{1}; };
        struct UnityRect { float x{0}, y{0}, w{0}, h{0}; };

        bool ReadCameraBackgroundColor(void* cam, UnityColor& outColor) const
        {
            if (mCameraGetBgColor_)
            {
                void* boxed = il2cpp_.Invoke(mCameraGetBgColor_, cam);
                void* raw = boxed ? il2cpp_.objectUnbox(boxed) : nullptr;
                if (!raw) return false;
                std::memcpy(&outColor, raw, sizeof(UnityColor));
                return true;
            }
            if (fnCameraGetBgInjected_ && cachedPtrField_ && il2cpp_.fieldGetValue)
            {
                void* nativePtr = nullptr;
                il2cpp_.fieldGetValue(cam, cachedPtrField_, &nativePtr);
                if (!nativePtr) return false;
                fnCameraGetBgInjected_(nativePtr, &outColor.r);
                return true;
            }
            return false;
        }

        bool WriteCameraBackgroundColor(void* cam, const UnityColor& color) const
        {
            if (mCameraSetBgColor_)
            {
                UnityColor copy = color;
                void* args[1]{&copy};
                return il2cpp_.InvokeVoid(mCameraSetBgColor_, cam, args);
            }
            if (fnCameraSetBgInjected_ && cachedPtrField_ && il2cpp_.fieldGetValue)
            {
                void* nativePtr = nullptr;
                il2cpp_.fieldGetValue(cam, cachedPtrField_, &nativePtr);
                if (!nativePtr) return false;
                fnCameraSetBgInjected_(nativePtr, &color.r);
                return true;
            }
            return false;
        }

        void* AllocateTempRenderTexture(uint32_t width, uint32_t height) const
        {
            int w = static_cast<int>(width);
            int h = static_cast<int>(height);
            int depth = 24;
            if (mRtGetTemporary5_)
            {
                int fmt = enumRtArgb32_;
                int rw = enumRtSrgb_;
                void* args[5]{&w, &h, &depth, &fmt, &rw};
                if (void* rt = il2cpp_.Invoke(mRtGetTemporary5_, nullptr, args)) return rt;
            }
            if (mRtGetTemporary4_)
            {
                int fmt = enumRtArgb32_;
                void* args[4]{&w, &h, &depth, &fmt};
                if (void* rt = il2cpp_.Invoke(mRtGetTemporary4_, nullptr, args)) return rt;
            }
            if (mRtGetTemporary3_)
            {
                void* args[3]{&w, &h, &depth};
                if (void* rt = il2cpp_.Invoke(mRtGetTemporary3_, nullptr, args)) return rt;
            }
            if (mRtGetTemporary2_)
            {
                void* args[2]{&w, &h};
                if (void* rt = il2cpp_.Invoke(mRtGetTemporary2_, nullptr, args)) return rt;
            }
            return nullptr;
        }

        static float HalfToFloat(uint16_t h) noexcept
        {
            const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
            const uint32_t exp = (h >> 10) & 0x1Fu;
            const uint32_t mant = h & 0x03FFu;
            uint32_t bits = 0;
            if (exp == 0)
            {
                if (mant == 0) bits = sign;
                else
                {
                    uint32_t m = mant;
                    int e = -1;
                    do { e++; m <<= 1; } while ((m & 0x0400u) == 0);
                    m &= 0x03FFu;
                    bits = sign | (static_cast<uint32_t>(127 - 15 - e) << 23) | (m << 13);
                }
            }
            else if (exp == 31)
            {
                bits = sign | 0x7F800000u | (mant << 13);
            }
            else
            {
                bits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
            }
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            return f;
        }

        static uint8_t LinearToSrgbByte(float linear) noexcept
        {
            if (!std::isfinite(linear) || linear <= 0.0f) return 0u;
            if (linear >= 1.0f) return 255u;
            const float srgb = (linear <= 0.0031308f)
                ? (linear * 12.92f)
                : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
            const long v = std::lround(std::clamp(srgb, 0.0f, 1.0f) * 255.0f);
            return static_cast<uint8_t>(std::clamp(v, 0L, 255L));
        }

        static DXGI_FORMAT ResolveCopyFormat(DXGI_FORMAT fmt) noexcept
        {
            switch (fmt)
            {
            case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            default: return fmt;
            }
        }

        static bool IsSupportedDxgiFormat(DXGI_FORMAT fmt) noexcept
        {
            switch (fmt)
            {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                return true;
            default:
                return false;
            }
        }

        void ReleaseGpuPools()
        {
            stagingTex_.Reset();
            resolveTex_.Reset();
            stagingDevice_.Reset();
            stagingW_ = stagingH_ = 0;
            stagingFmt_ = DXGI_FORMAT_UNKNOWN;
            resolveW_ = resolveH_ = 0;
            resolveFmt_ = DXGI_FORMAT_UNKNOWN;
        }

        bool ReadbackTextureD3D11(void* rt, uint32_t width, uint32_t height,
            std::vector<uint8_t>& outRgba, bool& outFlipVertical)
        {
            if (!rt || !mTexGetNativePtr_) return false;
            void* nativePtr = nullptr;
            if (!il2cpp_.ReadPtr(mTexGetNativePtr_, rt, nativePtr) || !nativePtr) return false;

            IUnknown* unk = static_cast<IUnknown*>(nativePtr);
            Microsoft::WRL::ComPtr<ID3D11Texture2D> srcTex;
            if (FAILED(unk->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(srcTex.GetAddressOf()))))
            {
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
                if (FAILED(unk->QueryInterface(__uuidof(ID3D11ShaderResourceView), reinterpret_cast<void**>(srv.GetAddressOf()))) || !srv)
                    return false;
                Microsoft::WRL::ComPtr<ID3D11Resource> res;
                srv->GetResource(res.GetAddressOf());
                if (!res || FAILED(res.As(&srcTex)))
                    return false;
            }
            if (!srcTex) return false;

            D3D11_TEXTURE2D_DESC desc{};
            srcTex->GetDesc(&desc);
            if (desc.Width != width || desc.Height != height || !IsSupportedDxgiFormat(desc.Format))
                return false;

            Microsoft::WRL::ComPtr<ID3D11Device> device;
            srcTex->GetDevice(device.GetAddressOf());
            if (!device) return false;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
            device->GetImmediateContext(ctx.GetAddressOf());
            if (!ctx) return false;

            const DXGI_FORMAT copyFmt = ResolveCopyFormat(desc.Format);
            ID3D11Texture2D* copySrc = srcTex.Get();

            if (desc.SampleDesc.Count > 1u)
            {
                if (!resolveTex_ || stagingDevice_.Get() != device.Get() ||
                    resolveW_ != width || resolveH_ != height || resolveFmt_ != copyFmt)
                {
                    resolveTex_.Reset();
                    D3D11_TEXTURE2D_DESC rDesc{};
                    rDesc.Width = width;
                    rDesc.Height = height;
                    rDesc.MipLevels = 1;
                    rDesc.ArraySize = 1;
                    rDesc.Format = copyFmt;
                    rDesc.SampleDesc.Count = 1;
                    rDesc.Usage = D3D11_USAGE_DEFAULT;
                    if (FAILED(device->CreateTexture2D(&rDesc, nullptr, resolveTex_.GetAddressOf())))
                        return false;
                    resolveW_ = width;
                    resolveH_ = height;
                    resolveFmt_ = copyFmt;
                }
                ctx->ResolveSubresource(resolveTex_.Get(), 0, srcTex.Get(), 0, copyFmt);
                copySrc = resolveTex_.Get();
            }

            if (!stagingTex_ || stagingDevice_.Get() != device.Get() ||
                stagingW_ != width || stagingH_ != height || stagingFmt_ != copyFmt)
            {
                stagingTex_.Reset();
                if (stagingDevice_.Get() != device.Get()) resolveTex_.Reset();
                stagingDevice_ = device;
                D3D11_TEXTURE2D_DESC sDesc{};
                sDesc.Width = width;
                sDesc.Height = height;
                sDesc.MipLevels = 1;
                sDesc.ArraySize = 1;
                sDesc.Format = copyFmt;
                sDesc.SampleDesc.Count = 1;
                sDesc.Usage = D3D11_USAGE_STAGING;
                sDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(device->CreateTexture2D(&sDesc, nullptr, stagingTex_.GetAddressOf())))
                {
                    ReleaseGpuPools();
                    return false;
                }
                stagingW_ = width;
                stagingH_ = height;
                stagingFmt_ = copyFmt;
            }

            ctx->CopySubresourceRegion(stagingTex_.Get(), 0, 0, 0, 0, copySrc, 0, nullptr);

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(ctx->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
                return false;

            outRgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
            const uint8_t* srcBase = static_cast<const uint8_t*>(mapped.pData);

            if (copyFmt == DXGI_FORMAT_R8G8B8A8_UNORM || copyFmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            {
                const size_t rowBytes = static_cast<size_t>(width) * 4u;
                for (uint32_t y = 0; y < height; ++y)
                {
                    std::memcpy(outRgba.data() + static_cast<size_t>(y) * rowBytes,
                        srcBase + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
                }
            }
            else if (copyFmt == DXGI_FORMAT_B8G8R8A8_UNORM || copyFmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            {
                for (uint32_t y = 0; y < height; ++y)
                {
                    const uint8_t* sRow = srcBase + static_cast<size_t>(y) * mapped.RowPitch;
                    uint8_t* dRow = outRgba.data() + static_cast<size_t>(y) * width * 4u;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const size_t idx = static_cast<size_t>(x) * 4u;
                        dRow[idx + 0] = sRow[idx + 2];
                        dRow[idx + 1] = sRow[idx + 1];
                        dRow[idx + 2] = sRow[idx + 0];
                        dRow[idx + 3] = sRow[idx + 3];
                    }
                }
            }
            else if (copyFmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
            {
                for (uint32_t y = 0; y < height; ++y)
                {
                    const uint16_t* sRow = reinterpret_cast<const uint16_t*>(
                        srcBase + static_cast<size_t>(y) * mapped.RowPitch);
                    uint8_t* dRow = outRgba.data() + static_cast<size_t>(y) * width * 4u;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const size_t idx = static_cast<size_t>(x) * 4u;
                        dRow[idx + 0] = LinearToSrgbByte(HalfToFloat(sRow[idx + 0]));
                        dRow[idx + 1] = LinearToSrgbByte(HalfToFloat(sRow[idx + 1]));
                        dRow[idx + 2] = LinearToSrgbByte(HalfToFloat(sRow[idx + 2]));
                        const float a = std::clamp(HalfToFloat(sRow[idx + 3]), 0.0f, 1.0f);
                        dRow[idx + 3] = static_cast<uint8_t>(std::lround(a * 255.0f));
                    }
                }
            }

            ctx->Unmap(stagingTex_.Get(), 0);
            outFlipVertical = false;
            return true;
        }

        bool ReadbackTexture2DFallback(void* rt, uint32_t width, uint32_t height,
            std::vector<uint8_t>& outRgba, bool& outFlipVertical)
        {
            if (!rt || !texture2DClass_ || !il2cpp_.objectNew || !il2cpp_.arrayLength ||
                !mRtGetActive_ || !mRtSetActive_ || !mTex2DGetRawBytes_)
                return false;

            const void* ctor = mTex2DCtor4_ ? mTex2DCtor4_ : mTex2DCtor2_;
            const void* readPixels = mTex2DReadPixels4_ ? mTex2DReadPixels4_ : mTex2DReadPixels3_;
            if (!ctor || !readPixels) return false;

            void* prevActive = il2cpp_.Invoke(mRtGetActive_);
            Il2CppDetail::ScopedGcPin prevPin(il2cpp_, prevActive);

            struct ActiveRtRestore
            {
                const Il2CppDetail::Il2CppApi& api;
                const void* setActive;
                void* prev;
                ~ActiveRtRestore()
                {
                    if (setActive)
                    {
                        void* args[1]{prev};
                        api.InvokeVoid(setActive, nullptr, args);
                    }
                }
            } activeRestore{il2cpp_, mRtSetActive_, prevActive};

            void* setArgs[1]{rt};
            if (!il2cpp_.InvokeVoid(mRtSetActive_, nullptr, setArgs)) return false;

            void* tex2D = il2cpp_.objectNew(texture2DClass_);
            if (!tex2D) return false;
            Il2CppDetail::ScopedGcPin texPin(il2cpp_, tex2D);

            int w = static_cast<int>(width);
            int h = static_cast<int>(height);
            int fmt = enumTexRgba32_;
            bool mip = false;
            bool constructed = false;
            if (mTex2DCtor4_)
            {
                void* cArgs[4]{&w, &h, &fmt, &mip};
                constructed = il2cpp_.InvokeVoid(mTex2DCtor4_, tex2D, cArgs);
            }
            else if (mTex2DCtor2_)
            {
                void* cArgs[2]{&w, &h};
                constructed = il2cpp_.InvokeVoid(mTex2DCtor2_, tex2D, cArgs);
            }
            if (!constructed) return false;

            struct DestroyTexGuard
            {
                const Il2CppDetail::Il2CppApi& api;
                const void* destroyMethod;
                void* obj;
                ~DestroyTexGuard()
                {
                    if (destroyMethod && obj)
                    {
                        void* args[1]{obj};
                        api.InvokeVoid(destroyMethod, nullptr, args);
                    }
                }
            } texDestroy{il2cpp_, mObjectDestroyImmediate_, tex2D};

            UnityRect rect{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
            int dstX = 0, dstY = 0;
            bool recalcMips = false;
            bool readOk = false;
            if (mTex2DReadPixels4_)
            {
                void* rArgs[4]{&rect, &dstX, &dstY, &recalcMips};
                readOk = il2cpp_.InvokeVoid(mTex2DReadPixels4_, tex2D, rArgs);
            }
            else
            {
                void* rArgs[3]{&rect, &dstX, &dstY};
                readOk = il2cpp_.InvokeVoid(mTex2DReadPixels3_, tex2D, rArgs);
            }
            if (!readOk) return false;

            void* byteArr = il2cpp_.Invoke(mTex2DGetRawBytes_, tex2D);
            if (!byteArr) return false;
            Il2CppDetail::ScopedGcPin arrPin(il2cpp_, byteArr);

            const size_t expectedBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
            const uintptr_t len = il2cpp_.arrayLength(byteArr);
            if (len < expectedBytes) return false;

            // IL2CPP SzArray header is 32 bytes on x64 (klass + monitor + bounds + max_length).
            const uint8_t* rawBytes = reinterpret_cast<const uint8_t*>(static_cast<const char*>(byteArr) + 32);
            outRgba.resize(expectedBytes);
            std::memcpy(outRgba.data(), rawBytes, expectedBytes);
            outFlipVertical = true;
            return true;
        }

        bool CaptureRawBuffers(uint32_t width, uint32_t height, bool dualPassAlpha,
            std::vector<uint8_t>& outBlack, std::vector<uint8_t>& outWhite,
            bool& outFlipVertical, std::string& outError)
        {
            {
                std::lock_guard<std::mutex> lock(testMutex_);
                if (testRender_)
                {
                    return testRender_(width, height, dualPassAlpha, outBlack, outWhite, outFlipVertical, outError);
                }
            }

            if (!unitySymbolsReady_)
            {
                outError = "Unity capture symbols unavailable";
                return false;
            }

            static thread_local bool t_capturing = false;
            if (t_capturing)
            {
                outError = "Reentrant capture prevented";
                return false;
            }
            t_capturing = true;
            struct ReentrancyReset { ~ReentrancyReset() { t_capturing = false; } } reentrancyReset;

            void* cam = il2cpp_.Invoke(mCameraGetMain_);
            if (!cam || !IsUnityAlive(cam))
            {
                outError = "Main camera is unavailable";
                return false;
            }
            Il2CppDetail::ScopedGcPin camPin(il2cpp_, cam);

            void* origTarget = il2cpp_.Invoke(mCameraGetTargetTex_, cam);
            Il2CppDetail::ScopedGcPin origTargetPin(il2cpp_, origTarget);

            float origAspect = 1.0f;
            const bool aspectSaved = il2cpp_.ReadFloat(mCameraGetAspect_, cam, origAspect);
            int origClearFlags = 1;
            const bool flagsSaved = il2cpp_.ReadInt(mCameraGetClearFlags_, cam, origClearFlags);
            UnityColor origBgColor{0, 0, 0, 1};
            const bool colorSaved = ReadCameraBackgroundColor(cam, origBgColor);

            void* urpAsset = nullptr;
            float origUrpScale = 1.0f;
            bool urpScaleModified = false;
            if (mGetCurrentPipeline_ && mUrpGetRenderScale_ && mUrpSetRenderScale_ &&
                urpAssetClass_ && il2cpp_.objectGetClass && il2cpp_.classIsAssignableFrom)
            {
                if (void* asset = il2cpp_.Invoke(mGetCurrentPipeline_))
                {
                    if (il2cpp_.classIsAssignableFrom(urpAssetClass_, il2cpp_.objectGetClass(asset)) &&
                        il2cpp_.ReadFloat(mUrpGetRenderScale_, asset, origUrpScale) &&
                        std::fabs(origUrpScale - 1.0f) > 1e-4f)
                    {
                        urpAsset = asset;
                    }
                }
            }
            Il2CppDetail::ScopedGcPin urpPin(il2cpp_, urpAsset);

            void* camData = nullptr;
            float origCamScale = 1.0f;
            bool camScaleModified = false;
            if (urpAdditionalCamTypeObj_ && mCompGetGameObject_ && mGoGetComponent_ &&
                mCamDataGetRenderScale_ && mCamDataSetRenderScale_)
            {
                if (void* go = il2cpp_.Invoke(mCompGetGameObject_, cam))
                {
                    void* cArgs[1]{urpAdditionalCamTypeObj_};
                    if (void* data = il2cpp_.Invoke(mGoGetComponent_, go, cArgs))
                    {
                        if (il2cpp_.ReadFloat(mCamDataGetRenderScale_, data, origCamScale) &&
                            std::fabs(origCamScale - 1.0f) > 1e-4f)
                        {
                            camData = data;
                        }
                    }
                }
            }
            Il2CppDetail::ScopedGcPin camDataPin(il2cpp_, camData);

            void* rt = AllocateTempRenderTexture(width, height);
            if (!rt)
            {
                outError = "RenderTexture.GetTemporary failed";
                return false;
            }
            Il2CppDetail::ScopedGcPin rtPin(il2cpp_, rt);

            if (mRtCreate_)
                il2cpp_.InvokeVoid(mRtCreate_, rt);

            struct CameraStateRestore
            {
                ScreenshotEngine& engine;
                void* cam;
                void* origTarget;
                bool aspectSaved;
                float origAspect;
                bool flagsSaved;
                int origClearFlags;
                bool colorSaved;
                UnityColor origBgColor;
                void* urpAsset;
                bool& urpScaleModified;
                float origUrpScale;
                void* camData;
                bool& camScaleModified;
                float origCamScale;
                void* rt;

                ~CameraStateRestore()
                {
                    if (colorSaved) engine.WriteCameraBackgroundColor(cam, origBgColor);
                    if (flagsSaved && engine.mCameraSetClearFlags_)
                    {
                        int f = origClearFlags;
                        void* fArgs[1]{&f};
                        engine.il2cpp_.InvokeVoid(engine.mCameraSetClearFlags_, cam, fArgs);
                    }
                    if (engine.mCameraSetTargetTex_)
                    {
                        void* tArgs[1]{origTarget};
                        engine.il2cpp_.InvokeVoid(engine.mCameraSetTargetTex_, cam, tArgs);
                    }
                    if (!origTarget && engine.mCameraResetAspect_)
                    {
                        engine.il2cpp_.InvokeVoid(engine.mCameraResetAspect_, cam);
                    }
                    else if (aspectSaved && engine.mCameraSetAspect_)
                    {
                        float a = origAspect;
                        void* aArgs[1]{&a};
                        engine.il2cpp_.InvokeVoid(engine.mCameraSetAspect_, cam, aArgs);
                    }
                    if (urpScaleModified && urpAsset && engine.mUrpSetRenderScale_)
                    {
                        float s = origUrpScale;
                        void* sArgs[1]{&s};
                        engine.il2cpp_.InvokeVoid(engine.mUrpSetRenderScale_, urpAsset, sArgs);
                    }
                    if (camScaleModified && camData && engine.mCamDataSetRenderScale_)
                    {
                        float s = origCamScale;
                        void* sArgs[1]{&s};
                        engine.il2cpp_.InvokeVoid(engine.mCamDataSetRenderScale_, camData, sArgs);
                    }
                    if (rt && engine.mRtReleaseTemporary_)
                    {
                        void* rArgs[1]{rt};
                        engine.il2cpp_.InvokeVoid(engine.mRtReleaseTemporary_, nullptr, rArgs);
                    }
                }
            } restore{
                *this, cam, origTarget, aspectSaved, origAspect, flagsSaved, origClearFlags,
                colorSaved, origBgColor, urpAsset, urpScaleModified, origUrpScale,
                camData, camScaleModified, origCamScale, rt
            };

            if (urpAsset)
            {
                float one = 1.0f;
                void* sArgs[1]{&one};
                urpScaleModified = il2cpp_.InvokeVoid(mUrpSetRenderScale_, urpAsset, sArgs);
            }
            if (camData)
            {
                float one = 1.0f;
                void* sArgs[1]{&one};
                camScaleModified = il2cpp_.InvokeVoid(mCamDataSetRenderScale_, camData, sArgs);
            }

            {
                void* tArgs[1]{rt};
                if (!il2cpp_.InvokeVoid(mCameraSetTargetTex_, cam, tArgs))
                {
                    outError = "Camera.set_targetTexture failed";
                    return false;
                }
            }
            {
                float targetAspect = static_cast<float>(width) / static_cast<float>(height);
                void* aArgs[1]{&targetAspect};
                il2cpp_.InvokeVoid(mCameraSetAspect_, cam, aArgs);
            }

            auto renderAndRead = [&](std::vector<uint8_t>& outPixels, bool& flip) -> bool {
                if (!il2cpp_.InvokeVoid(mCameraRender_, cam))
                {
                    outError = "Camera.Render raised an exception";
                    return false;
                }
                if (ReadbackTextureD3D11(rt, width, height, outPixels, flip)) return true;
                if (ReadbackTexture2DFallback(rt, width, height, outPixels, flip)) return true;
                outError = "GPU texture readback failed";
                return false;
            };

            if (!dualPassAlpha)
            {
                return renderAndRead(outBlack, outFlipVertical);
            }

            int solidFlags = enumClearSolidColor_;
            void* fArgs[1]{&solidFlags};
            if (!il2cpp_.InvokeVoid(mCameraSetClearFlags_, cam, fArgs))
            {
                outError = "Camera.set_clearFlags failed";
                return false;
            }

            const UnityColor blackClear{0.0f, 0.0f, 0.0f, 0.0f};
            if (!WriteCameraBackgroundColor(cam, blackClear))
            {
                outError = "Camera.set_backgroundColor(black) failed";
                return false;
            }
            bool flipBlack = false;
            if (!renderAndRead(outBlack, flipBlack)) return false;

            const UnityColor whiteClear{1.0f, 1.0f, 1.0f, 0.0f};
            if (!WriteCameraBackgroundColor(cam, whiteClear))
            {
                outError = "Camera.set_backgroundColor(white) failed";
                return false;
            }
            bool flipWhite = false;
            if (!renderAndRead(outWhite, flipWhite)) return false;

            if (flipBlack != flipWhite)
            {
                // Normalize orientation if readback paths somehow differed between passes.
                if (flipBlack) FlipRgbaRows(width, height, outBlack);
                if (flipWhite) FlipRgbaRows(width, height, outWhite);
                outFlipVertical = false;
            }
            else
            {
                outFlipVertical = flipBlack;
            }
            return true;
        }

        static void FlipRgbaRows(uint32_t width, uint32_t height, std::vector<uint8_t>& pixels)
        {
            const size_t rowBytes = static_cast<size_t>(width) * 4u;
            std::vector<uint8_t> tmp(rowBytes);
            for (uint32_t y = 0; y < height / 2u; ++y)
            {
                uint8_t* top = pixels.data() + static_cast<size_t>(y) * rowBytes;
                uint8_t* bot = pixels.data() + static_cast<size_t>(height - 1u - y) * rowBytes;
                std::memcpy(tmp.data(), top, rowBytes);
                std::memcpy(top, bot, rowBytes);
                std::memcpy(bot, tmp.data(), rowBytes);
            }
        }

        void ProcessPendingFileCapturesOnMainThread()
        {
            std::deque<std::shared_ptr<TicketState>> batch;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                if (pendingQueue_.empty()) return;
                batch.swap(pendingQueue_);
            }

            for (auto& ticket : batch)
            {
                if (!ticket || ticket->cancelled.load(std::memory_order_acquire)) continue;
                if (shuttingDown_.load(std::memory_order_acquire)) break;

                uint32_t w = ticket->requestedWidth;
                uint32_t h = ticket->requestedHeight;
                if (!ResolveTargetDimensions(w, h))
                {
                    FailTicket(*ticket, GAKU_E_UNAVAILABLE, "Target resolution or camera unavailable");
                    continue;
                }

                const bool wantAlpha = (ticket->imageFormat == GAKU_IMAGE_PNG) &&
                    ShouldCaptureAlpha(ticket->alphaMode);

                EncodeJob job;
                job.ticket = ticket;
                job.width = w;
                job.height = h;
                job.useAlpha = wantAlpha;

                std::string capErr;
                if (!CaptureRawBuffers(w, h, wantAlpha, job.blackRgba, job.whiteRgba, job.flipVertical, capErr))
                {
                    FailTicket(*ticket, GAKU_E_UNAVAILABLE, capErr);
                    continue;
                }

                activeCount_.fetch_add(1, std::memory_order_acq_rel);
                {
                    std::lock_guard<std::mutex> wLock(workerMutex_);
                    encodeQueue_.push_back(std::move(job));
                }
                workerCv_.notify_one();
            }
        }

        void FailTicket(TicketState& ticket, int32_t code, const std::string& msg)
        {
            {
                std::lock_guard<std::mutex> lock(ticketMutex_);
                ticket.state = GAKU_CAPTURE_FAILED;
                ticket.errorCode = code;
                ticket.errorMessage = msg;
            }
            {
                std::lock_guard<std::mutex> lock(recentMutex_);
                lastCaptureError_ = msg;
                CaptureCompletionNotice notice;
                notice.ticket = ticket.id;
                notice.ok = false;
                notice.pathUtf8 = ticket.savedPathUtf8;
                notice.fileNameUtf8 = PathToUtf8(ticket.savedPath.filename());
                notice.imageFormat = ticket.imageFormat;
                notice.errorMessage = msg;
                completionQueue_.push_back(std::move(notice));
                if (completionQueue_.size() > 32u) completionQueue_.pop_front();
            }
        }

        void WorkerLoop()
        {
            std::vector<uint8_t> finalRgba;
            for (;;)
            {
                EncodeJob job;
                {
                    std::unique_lock<std::mutex> lock(workerMutex_);
                    workerCv_.wait(lock, [this]() { return workerStop_ || !encodeQueue_.empty(); });
                    if (workerStop_ && encodeQueue_.empty()) break;
                    job = std::move(encodeQueue_.front());
                    encodeQueue_.pop_front();
                }

                struct ActiveGuard
                {
                    std::atomic<uint32_t>& cnt;
                    ~ActiveGuard() { cnt.fetch_sub(1, std::memory_order_acq_rel); }
                } activeGuard{activeCount_};

                if (!job.ticket || job.ticket->cancelled.load(std::memory_order_acquire)) continue;

                const size_t byteCount = static_cast<size_t>(job.width) * static_cast<size_t>(job.height) * 4u;
                finalRgba.resize(byteCount);

                bool converted = false;
                if (job.useAlpha)
                {
                    converted = AlphaMatte::ExtractAlphaDifferenceMatte(
                        job.width, job.height, job.blackRgba.data(), job.whiteRgba.data(),
                        finalRgba.data(), job.flipVertical);
                }
                else
                {
                    converted = AlphaMatte::CopyOpaqueToRgba(
                        job.width, job.height, job.blackRgba.data(),
                        job.flipVertical, finalRgba.data());
                }

                if (!converted)
                {
                    FailTicket(*job.ticket, GAKU_E_INTERNAL, "Pixel buffer conversion failed");
                    continue;
                }

                if (job.ticket->cancelled.load(std::memory_order_acquire)) continue;

                std::string writeErr;
                bool writeOk = false;
                if (job.ticket->imageFormat == GAKU_IMAGE_JPEG)
                {
                    writeOk = WicWriter::WriteJpegRgba(
                        job.ticket->savedPath, job.width, job.height,
                        finalRgba.data(), job.ticket->jpegQuality, &writeErr);
                }
                else
                {
                    writeOk = WicWriter::WritePngRgba(
                        job.ticket->savedPath, job.width, job.height,
                        finalRgba.data(), job.useAlpha, &writeErr);
                }

                if (!writeOk)
                {
                    FailTicket(*job.ticket, GAKU_E_INTERNAL, writeErr.empty() ? "Image encoding failed" : writeErr);
                    continue;
                }

                std::error_code ec;
                const uint64_t fileSize = static_cast<uint64_t>(
                    std::filesystem::file_size(job.ticket->savedPath, ec));
                const std::string timestamp = FormatTimestampNow();
                const std::string fileName = PathToUtf8(job.ticket->savedPath.filename());

                {
                    std::lock_guard<std::mutex> lock(ticketMutex_);
                    job.ticket->state = GAKU_CAPTURE_SUCCEEDED;
                    job.ticket->width = job.width;
                    job.ticket->height = job.height;
                    job.ticket->hasAlpha = job.useAlpha;
                    job.ticket->errorCode = GAKU_OK;
                    job.ticket->errorMessage.clear();
                }

                {
                    std::lock_guard<std::mutex> lock(recentMutex_);
                    lastCaptureError_.clear();

                    RecentCaptureEntry entry;
                    entry.id = nextRecentId_.fetch_add(1, std::memory_order_relaxed);
                    entry.pathUtf8 = job.ticket->savedPathUtf8;
                    entry.fileNameUtf8 = fileName;
                    entry.width = job.width;
                    entry.height = job.height;
                    entry.imageFormat = job.ticket->imageFormat;
                    entry.hasAlpha = job.useAlpha;
                    entry.sizeBytes = ec ? 0ull : fileSize;
                    entry.timestamp = timestamp;
                    recentCaptures_.insert(recentCaptures_.begin(), entry);
                    if (recentCaptures_.size() > 20u) recentCaptures_.resize(20u);

                    CaptureCompletionNotice notice;
                    notice.ticket = job.ticket->id;
                    notice.ok = true;
                    notice.pathUtf8 = entry.pathUtf8;
                    notice.fileNameUtf8 = fileName;
                    notice.width = job.width;
                    notice.height = job.height;
                    notice.imageFormat = entry.imageFormat;
                    notice.hasAlpha = job.useAlpha;
                    notice.sizeBytes = entry.sizeBytes;
                    notice.timestamp = timestamp;
                    completionQueue_.push_back(std::move(notice));
                    if (completionQueue_.size() > 32u) completionQueue_.pop_front();
                }
            }
        }

        // Host services
        mutable std::mutex hostMutex_;
        GakuHostApi hostApi_{};
        bool hostApiSet_{false};
        GakuLogService logService_{};
        GakuMainThreadService mainThreadService_{};
        GakuMotionStageStateServiceV1 stageStateService_{};
        bool stageStateCached_{false};

        // Runtime & capabilities state
        std::atomic<bool> shuttingDown_{false};
        std::atomic<bool> runtimeReady_{false};
        std::atomic<bool> cachedCameraAvailable_{false};
        std::atomic<uint32_t> cachedWindowWidth_{0};
        std::atomic<uint32_t> cachedWindowHeight_{0};
        std::atomic<int> testGreenScreenOverride_{-1};
        std::atomic<bool> testMainThreadBypass_{false};

        // Test hook
        std::mutex testMutex_;
        TestRenderFn testRender_;

        // IL2CPP metadata & methods
        std::mutex il2cppResolveMutex_;
        bool il2cppResolved_{false};
        bool unitySymbolsReady_{false};
        Il2CppDetail::Il2CppApi il2cpp_{};

        void* objectClass_{nullptr};
        void* cameraClass_{nullptr};
        void* componentClass_{nullptr};
        void* gameObjectClass_{nullptr};
        void* renderTextureClass_{nullptr};
        void* textureClass_{nullptr};
        void* texture2DClass_{nullptr};
        void* screenClass_{nullptr};
        void* timeClass_{nullptr};
        void* colorClass_{nullptr};
        void* rectClass_{nullptr};
        void* clearFlagsClass_{nullptr};
        void* rtFormatClass_{nullptr};
        void* rtReadWriteClass_{nullptr};
        void* texFormatClass_{nullptr};
        void* graphicsSettingsClass_{nullptr};
        void* renderPipelineAssetClass_{nullptr};
        void* urpAssetClass_{nullptr};
        void* urpAdditionalCamClass_{nullptr};
        void* typeClass_{nullptr};
        void* byteArrayClass_{nullptr};

        std::atomic<bool> pendingCamDataType_{false};
        void* urpAdditionalCamTypeObj_{nullptr};

        int enumClearSolidColor_{2};
        int enumRtArgb32_{0};
        int enumRtSrgb_{2};
        int enumTexRgba32_{4};

        const void* mObjectAlive_{nullptr};
        const void* mObjectDestroyImmediate_{nullptr};
        const void* mCameraGetMain_{nullptr};
        const void* mCameraGetTargetTex_{nullptr};
        const void* mCameraSetTargetTex_{nullptr};
        const void* mCameraGetAspect_{nullptr};
        const void* mCameraSetAspect_{nullptr};
        const void* mCameraResetAspect_{nullptr};
        const void* mCameraGetClearFlags_{nullptr};
        const void* mCameraSetClearFlags_{nullptr};
        const void* mCameraGetBgColor_{nullptr};
        const void* mCameraSetBgColor_{nullptr};
        const void* mCameraRender_{nullptr};
        const void* mCameraGetPixelWidth_{nullptr};
        const void* mCameraGetPixelHeight_{nullptr};

        void* cachedPtrField_{nullptr};
        void (*fnCameraGetBgInjected_)(void*, float*){nullptr};
        void (*fnCameraSetBgInjected_)(void*, const float*){nullptr};

        const void* mRtGetTemporary5_{nullptr};
        const void* mRtGetTemporary4_{nullptr};
        const void* mRtGetTemporary3_{nullptr};
        const void* mRtGetTemporary2_{nullptr};
        const void* mRtReleaseTemporary_{nullptr};
        const void* mRtGetActive_{nullptr};
        const void* mRtSetActive_{nullptr};
        const void* mRtCreate_{nullptr};

        const void* mTexGetNativePtr_{nullptr};
        const void* mTex2DCtor4_{nullptr};
        const void* mTex2DCtor2_{nullptr};
        const void* mTex2DReadPixels4_{nullptr};
        const void* mTex2DReadPixels3_{nullptr};
        const void* mTex2DGetRawBytes_{nullptr};

        const void* mScreenGetWidth_{nullptr};
        const void* mScreenGetHeight_{nullptr};
        const void* mTimeGetFrameCount_{nullptr};

        const void* mGetCurrentPipeline_{nullptr};
        const void* mUrpGetRenderScale_{nullptr};
        const void* mUrpSetRenderScale_{nullptr};
        const void* mCompGetGameObject_{nullptr};
        const void* mGoGetComponent_{nullptr};
        const void* mCamDataGetRenderScale_{nullptr};
        const void* mCamDataSetRenderScale_{nullptr};

        // Pooled GPU & immediate readback buffers (Unity main thread only)
        Microsoft::WRL::ComPtr<ID3D11Device> stagingDevice_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTex_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> resolveTex_;
        uint32_t stagingW_{0}, stagingH_{0};
        DXGI_FORMAT stagingFmt_{DXGI_FORMAT_UNKNOWN};
        uint32_t resolveW_{0}, resolveH_{0};
        DXGI_FORMAT resolveFmt_{DXGI_FORMAT_UNKNOWN};
        std::vector<uint8_t> immediateBlack_;
        std::vector<uint8_t> immediateWhite_;

        // Tickets, queues, and worker thread
        std::mutex ticketMutex_;
        std::unordered_map<uint64_t, std::shared_ptr<TicketState>> tickets_;
        std::atomic<uint64_t> nextTicketId_{1};

        std::mutex pendingMutex_;
        std::deque<std::shared_ptr<TicketState>> pendingQueue_;

        std::mutex workerMutex_;
        std::condition_variable workerCv_;
        std::deque<EncodeJob> encodeQueue_;
        std::thread workerThread_;
        bool workerStarted_{false};
        bool workerStop_{false};
        std::atomic<uint32_t> activeCount_{0};

        std::mutex recentMutex_;
        std::vector<RecentCaptureEntry> recentCaptures_;
        std::deque<CaptureCompletionNotice> completionQueue_;
        std::string lastCaptureError_;
        std::atomic<uint64_t> nextRecentId_{1};
    };
} // namespace ScreenshotManager
