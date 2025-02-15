#include <stdarg.h>
#include <stdbool.h>
#include <assert.h>

#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"
#include "wine/heap.h"

#include "wine/vulkan.h"
#include "wine/asm.h"

#define COBJMACROS
#include "d3d11.h"
#include "d3d12.h"

#include "amd_ags.h"

WINE_DEFAULT_DEBUG_CHANNEL(amd_ags);

enum amd_ags_version
{
    AMD_AGS_VERSION_5_1_1,
    AMD_AGS_VERSION_5_2_0,
    AMD_AGS_VERSION_5_2_1,
    AMD_AGS_VERSION_5_3_0,
    AMD_AGS_VERSION_5_4_0,
    AMD_AGS_VERSION_5_4_1,
    AMD_AGS_VERSION_5_4_2,
    AMD_AGS_VERSION_6_0_0,
    AMD_AGS_VERSION_6_0_1,

    AMD_AGS_VERSION_COUNT
};

static const struct
{
    int major;
    int minor;
    int patch;
    unsigned int device_size;
    unsigned int dx11_returned_params_size;
}
amd_ags_info[AMD_AGS_VERSION_COUNT] =
{
    {5, 1, 1, sizeof(AGSDeviceInfo_511), sizeof(AGSDX11ReturnedParams_511)},
    {5, 2, 0, sizeof(AGSDeviceInfo_520), sizeof(AGSDX11ReturnedParams_520)},
    {5, 2, 1, sizeof(AGSDeviceInfo_520), sizeof(AGSDX11ReturnedParams_520)},
    {5, 3, 0, sizeof(AGSDeviceInfo_520), sizeof(AGSDX11ReturnedParams_520)},
    {5, 4, 0, sizeof(AGSDeviceInfo_540), sizeof(AGSDX11ReturnedParams_520)},
    {5, 4, 1, sizeof(AGSDeviceInfo_541), sizeof(AGSDX11ReturnedParams_520)},
    {5, 4, 2, sizeof(AGSDeviceInfo_542), sizeof(AGSDX11ReturnedParams_520)},
    {6, 0, 0, sizeof(AGSDeviceInfo_600), sizeof(AGSDX11ReturnedParams_600)},
    {6, 0, 1, sizeof(AGSDeviceInfo_600), sizeof(AGSDX11ReturnedParams_600)},
};

#define DEF_FIELD(name) {DEVICE_FIELD_##name, {offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_520, name), \
        offsetof(AGSDeviceInfo_520, name), offsetof(AGSDeviceInfo_520, name), offsetof(AGSDeviceInfo_540, name), \
        offsetof(AGSDeviceInfo_541, name), offsetof(AGSDeviceInfo_542, name), offsetof(AGSDeviceInfo_600, name), \
        offsetof(AGSDeviceInfo_600, name)}}
#define DEF_FIELD_520_BELOW(name) {DEVICE_FIELD_##name, {offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_520, name), \
        offsetof(AGSDeviceInfo_520, name), offsetof(AGSDeviceInfo_520, name), -1, \
        -1, -1, -1, -1}}
#define DEF_FIELD_540_UP(name) {DEVICE_FIELD_##name, {-1, -1, \
        -1, -1, offsetof(AGSDeviceInfo_540, name), \
        offsetof(AGSDeviceInfo_541, name), offsetof(AGSDeviceInfo_542, name), offsetof(AGSDeviceInfo_600, name), \
        offsetof(AGSDeviceInfo_600, name)}}
#define DEF_FIELD_600_BELOW(name) {DEVICE_FIELD_##name, {offsetof(AGSDeviceInfo_511, name), offsetof(AGSDeviceInfo_520, name), \
        offsetof(AGSDeviceInfo_520, name), offsetof(AGSDeviceInfo_520, name), offsetof(AGSDeviceInfo_540, name), \
        offsetof(AGSDeviceInfo_541, name), offsetof(AGSDeviceInfo_542, name), -1, \
        -1}}

#define DEVICE_FIELD_adapterString 0
#define DEVICE_FIELD_architectureVersion 1
#define DEVICE_FIELD_asicFamily 2
#define DEVICE_FIELD_vendorId 3
#define DEVICE_FIELD_deviceId 4
#define DEVICE_FIELD_isPrimaryDevice 5
#define DEVICE_FIELD_localMemoryInBytes 6
#define DEVICE_FIELD_numDisplays 7
#define DEVICE_FIELD_displays 8

static const struct
{
    unsigned int field_index;
    int offset[AMD_AGS_VERSION_COUNT];
}
device_struct_fields[] =
{
    DEF_FIELD(adapterString),
    DEF_FIELD_520_BELOW(architectureVersion),
    DEF_FIELD_540_UP(asicFamily),
    DEF_FIELD(vendorId),
    DEF_FIELD(deviceId),
    DEF_FIELD_600_BELOW(isPrimaryDevice),
    DEF_FIELD(localMemoryInBytes),
    DEF_FIELD(numDisplays),
    DEF_FIELD(displays),
};

#undef DEF_FIELD

#define GET_DEVICE_FIELD_ADDR(device, name, type, version) \
        (device_struct_fields[DEVICE_FIELD_##name].offset[version] == -1 ? NULL \
        : (type *)((BYTE *)device + device_struct_fields[DEVICE_FIELD_##name].offset[version]))

#define SET_DEVICE_FIELD(device, name, type, version, value) { \
        type *addr; \
        if ((addr = GET_DEVICE_FIELD_ADDR(device, name, type, version))) \
            *addr = value; \
    }

struct AGSContext
{
    enum amd_ags_version version;
    unsigned int device_count;
    struct AGSDeviceInfo *devices;
    VkPhysicalDeviceProperties *properties;
    VkPhysicalDeviceMemoryProperties *memory_properties;
};

static HMODULE hd3d11, hd3d12;
static typeof(D3D12CreateDevice) *pD3D12CreateDevice;
static typeof(D3D11CreateDevice) *pD3D11CreateDevice;
static typeof(D3D11CreateDeviceAndSwapChain) *pD3D11CreateDeviceAndSwapChain;

static BOOL load_d3d12_functions(void)
{
    if (hd3d12)
        return TRUE;

    if (!(hd3d12 = LoadLibraryA("d3d12.dll")))
        return FALSE;

    pD3D12CreateDevice = (void *)GetProcAddress(hd3d12, "D3D12CreateDevice");
    return TRUE;
}

static BOOL load_d3d11_functions(void)
{
    if (hd3d11)
        return TRUE;

    if (!(hd3d11 = LoadLibraryA("d3d11.dll")))
        return FALSE;

    pD3D11CreateDevice = (void *)GetProcAddress(hd3d11, "D3D11CreateDevice");
    pD3D11CreateDeviceAndSwapChain = (void *)GetProcAddress(hd3d11, "D3D11CreateDeviceAndSwapChain");
    return TRUE;
}

static AGSReturnCode vk_get_physical_device_properties(unsigned int *out_count,
        VkPhysicalDeviceProperties **out, VkPhysicalDeviceMemoryProperties **out_memory)
{
    VkPhysicalDeviceProperties *properties = NULL;
    VkPhysicalDeviceMemoryProperties *memory_properties = NULL;
    VkPhysicalDevice *vk_physical_devices = NULL;
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkInstanceCreateInfo create_info;
    AGSReturnCode ret = AGS_SUCCESS;
    uint32_t count, i;
    VkResult vr;

    *out = NULL;
    *out_count = 0;

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    if ((vr = vkCreateInstance(&create_info, NULL, &vk_instance) < 0))
    {
        WARN("Failed to create Vulkan instance, vr %d.\n", vr);
        goto done;
    }

    if ((vr = vkEnumeratePhysicalDevices(vk_instance, &count, NULL)) < 0)
    {
        WARN("Failed to enumerate devices, vr %d.\n", vr);
        goto done;
    }

    if (!(vk_physical_devices = heap_calloc(count, sizeof(*vk_physical_devices))))
    {
        WARN("Failed to allocate memory.\n");
        ret = AGS_OUT_OF_MEMORY;
        goto done;
    }

    if ((vr = vkEnumeratePhysicalDevices(vk_instance, &count, vk_physical_devices)) < 0)
    {
        WARN("Failed to enumerate devices, vr %d.\n", vr);
        goto done;
    }

    if (!(properties = heap_calloc(count, sizeof(*properties))))
    {
        WARN("Failed to allocate memory.\n");
        ret = AGS_OUT_OF_MEMORY;
        goto done;
    }

    if (!(memory_properties = heap_calloc(count, sizeof(*memory_properties))))
    {
        WARN("Failed to allocate memory.\n");
        heap_free(properties);
        ret = AGS_OUT_OF_MEMORY;
        goto done;
    }

    for (i = 0; i < count; ++i)
        vkGetPhysicalDeviceProperties(vk_physical_devices[i], &properties[i]);

    for (i = 0; i < count; ++i)
        vkGetPhysicalDeviceMemoryProperties(vk_physical_devices[i], &memory_properties[i]);

    *out_count = count;
    *out = properties;
    *out_memory = memory_properties;

done:
    heap_free(vk_physical_devices);
    if (vk_instance)
        vkDestroyInstance(vk_instance, NULL);
    return ret;
}

static enum amd_ags_version determine_ags_version(void)
{
    /* AMD AGS is not binary compatible between versions (even minor versions), and the game
     * does not request a specific version when calling agsInit().
     * Checking the version of amd_ags_x64.dll shipped with the game is the only way to
     * determine what version the game was built against.
     *
     * An update to AGS 5.4.1 included an amd_ags_x64.dll with no file version info.
     * In case of an error, assume it's that version.
     */
    enum amd_ags_version ret = AMD_AGS_VERSION_5_4_1;
    DWORD infosize;
    void *infobuf = NULL;
    void *val;
    UINT vallen, i;
    VS_FIXEDFILEINFO *info;
    UINT16 major, minor, patch;
    WCHAR dllname[MAX_PATH], temp_path[MAX_PATH], temp_name[MAX_PATH];

    *temp_name = 0;
    if (!(infosize = GetModuleFileNameW(GetModuleHandleW(L"amd_ags_x64.dll"), dllname, ARRAY_SIZE(dllname)))
            || infosize == ARRAY_SIZE(dllname))
    {
        ERR("GetModuleFileNameW failed.\n");
        goto done;
    }
    if (!GetTempPathW(MAX_PATH, temp_path) || !GetTempFileNameW(temp_path, L"tmp", 0, temp_name))
    {
        ERR("Failed getting temp file name.\n");
        goto done;
    }
    if (!CopyFileW(dllname, temp_name, FALSE))
    {
        ERR("Failed to copy file.\n");
        goto done;
    }

    infosize = GetFileVersionInfoSizeW(temp_name, NULL);
    if (!infosize)
    {
        ERR("Unable to determine desired version of amd_ags_x64.dll.\n");
        goto done;
    }

    if (!(infobuf = heap_alloc(infosize)))
    {
        ERR("Failed to allocate memory.\n");
        goto done;
    }

    if (!GetFileVersionInfoW(temp_name, 0, infosize, infobuf))
    {
        ERR("Unable to determine desired version of amd_ags_x64.dll.\n");
        goto done;
    }

    if (!VerQueryValueW(infobuf, L"\\", &val, &vallen) || (vallen != sizeof(VS_FIXEDFILEINFO)))
    {
        ERR("Unable to determine desired version of amd_ags_x64.dll.\n");
        goto done;
    }

    info = val;
    major = info->dwFileVersionMS >> 16;
    minor = info->dwFileVersionMS;
    patch = info->dwFileVersionLS >> 16;
    TRACE("Found amd_ags_x64.dll v%d.%d.%d\n", major, minor, patch);

    for (i = 0; i < ARRAY_SIZE(amd_ags_info); i++)
    {
        if ((major == amd_ags_info[i].major) &&
            (minor == amd_ags_info[i].minor) &&
            (patch == amd_ags_info[i].patch))
        {
            ret = i;
            break;
        }
    }

done:
    if (*temp_name)
        DeleteFileW(temp_name);

    heap_free(infobuf);
    TRACE("Using AGS v%d.%d.%d interface\n",
          amd_ags_info[ret].major, amd_ags_info[ret].minor, amd_ags_info[ret].patch);
    return ret;
}

struct monitor_enum_context_600
{
    const char *adapter_name;
    AGSDisplayInfo_600 **ret_displays;
    int *ret_display_count;
};

static BOOL WINAPI monitor_enum_proc_600(HMONITOR hmonitor, HDC hdc, RECT *rect, LPARAM context)
{
    struct monitor_enum_context_600 *c = (struct monitor_enum_context_600 *)context;
    MONITORINFOEXA monitor_info;
    AGSDisplayInfo_600 *new_alloc;
    DISPLAY_DEVICEA device;
    AGSDisplayInfo_600 *info;
    unsigned int i, mode;
    DEVMODEA dev_mode;


    monitor_info.cbSize = sizeof(monitor_info);
    GetMonitorInfoA(hmonitor, (MONITORINFO *)&monitor_info);
    TRACE("monitor_info.szDevice %s.\n", debugstr_a(monitor_info.szDevice));

    device.cb = sizeof(device);
    i = 0;
    while (EnumDisplayDevicesA(NULL, i, &device, 0))
    {
        TRACE("device.DeviceName %s, device.DeviceString %s.\n", debugstr_a(device.DeviceName), debugstr_a(device.DeviceString));
        ++i;
        if (strcmp(device.DeviceString, c->adapter_name) || strcmp(device.DeviceName, monitor_info.szDevice))
            continue;

        if (*c->ret_display_count)
        {
            if (!(new_alloc = heap_realloc(*c->ret_displays, sizeof(*new_alloc) * (*c->ret_display_count + 1))))
            {
                ERR("No memory.");
                return FALSE;
            }
            *c->ret_displays = new_alloc;
        }
        else if (!(*c->ret_displays = heap_alloc(sizeof(**c->ret_displays))))
        {
            ERR("No memory.");
            return FALSE;
        }
        info = &(*c->ret_displays)[*c->ret_display_count];
        memset(info, 0, sizeof(*info));
        strcpy(info->displayDeviceName, device.DeviceName);
        if (EnumDisplayDevicesA(info->displayDeviceName, 0, &device, 0))
        {
            strcpy(info->name, device.DeviceString);
        }
        else
        {
            ERR("Could not get monitor name for device %s.\n", debugstr_a(info->displayDeviceName));
            strcpy(info->name, "Unknown");
        }
        if (monitor_info.dwFlags & MONITORINFOF_PRIMARY)
            info->isPrimaryDisplay = 1;

        mode = 0;
        memset(&dev_mode, 0, sizeof(dev_mode));
        dev_mode.dmSize = sizeof(dev_mode);
        while (EnumDisplaySettingsExA(monitor_info.szDevice, mode, &dev_mode, EDS_RAWMODE))
        {
            ++mode;
            if (dev_mode.dmPelsWidth > info->maxResolutionX)
                info->maxResolutionX = dev_mode.dmPelsWidth;
            if (dev_mode.dmPelsHeight > info->maxResolutionY)
                info->maxResolutionY = dev_mode.dmPelsHeight;
            if (dev_mode.dmDisplayFrequency > info->maxRefreshRate)
                info->maxRefreshRate = dev_mode.dmDisplayFrequency;
            memset(&dev_mode, 0, sizeof(dev_mode));
            dev_mode.dmSize = sizeof(dev_mode);
        }

        info->currentResolution.offsetX = monitor_info.rcMonitor.left;
        info->currentResolution.offsetY = monitor_info.rcMonitor.top;
        info->currentResolution.width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
        info->currentResolution.height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
        info->visibleResolution = info->currentResolution;

        memset(&dev_mode, 0, sizeof(dev_mode));
        dev_mode.dmSize = sizeof(dev_mode);

        if (EnumDisplaySettingsExA(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &dev_mode, EDS_RAWMODE))
            info->currentRefreshRate = dev_mode.dmDisplayFrequency;
        else
            ERR("Could not get current display settings.\n");
        ++*c->ret_display_count;

        TRACE("Added display %s for %s.\n", debugstr_a(monitor_info.szDevice), debugstr_a(c->adapter_name));
    }

    return TRUE;
}

static void init_device_displays_600(const char *adapter_name, AGSDisplayInfo_600 **ret_displays, int *ret_display_count)
{
    struct monitor_enum_context_600 context;

    TRACE("adapter_name %s.\n", debugstr_a(adapter_name));

    context.adapter_name = adapter_name;
    context.ret_displays = ret_displays;
    context.ret_display_count = ret_display_count;

    EnumDisplayMonitors(NULL, NULL, monitor_enum_proc_600, (LPARAM)&context);
}

static void init_device_displays_511(const char *adapter_name, AGSDisplayInfo_511 **ret_displays, int *ret_display_count)
{
    AGSDisplayInfo_600 *displays = NULL;
    int display_count = 0;
    int i;
    *ret_displays = NULL;
    *ret_display_count = 0;

    init_device_displays_600(adapter_name, &displays, &display_count);

    if ((*ret_displays = heap_alloc(sizeof(**ret_displays) * display_count)))
    {
        for (i = 0; i < display_count; i++)
        {
            memcpy(&(*ret_displays)[i], &displays[i], sizeof(AGSDisplayInfo_511));
        }
        *ret_display_count = display_count;
    }

    heap_free(displays);
}


static AGSReturnCode init_ags_context(AGSContext *context)
{
    AGSReturnCode ret;
    unsigned int i, j;
    BYTE *device;

    context->version = determine_ags_version();
    context->device_count = 0;
    context->devices = NULL;
    context->properties = NULL;
    context->memory_properties = NULL;

    ret = vk_get_physical_device_properties(&context->device_count, &context->properties, &context->memory_properties);
    if (ret != AGS_SUCCESS || !context->device_count)
        return ret;

    assert(context->version < AMD_AGS_VERSION_COUNT);

    if (!(context->devices = heap_calloc(context->device_count, amd_ags_info[context->version].device_size)))
    {
        WARN("Failed to allocate memory.\n");
        heap_free(context->properties);
        heap_free(context->memory_properties);
        return AGS_OUT_OF_MEMORY;
    }

    device = (BYTE *)context->devices;
    for (i = 0; i < context->device_count; ++i)
    {
        const VkPhysicalDeviceProperties *vk_properties = &context->properties[i];
        const VkPhysicalDeviceMemoryProperties *vk_memory_properties = &context->memory_properties[i];
        VkDeviceSize local_memory_size = 0;

        for (j = 0; j < vk_memory_properties->memoryHeapCount; j++)
        {
            if (vk_memory_properties->memoryHeaps[j].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                local_memory_size = vk_memory_properties->memoryHeaps[j].size;
                break;
            }
        }
        TRACE("device %s, %04x:%04x, reporting local memory size 0x%s bytes\n", debugstr_a(vk_properties->deviceName),
                vk_properties->vendorID, vk_properties->deviceID, wine_dbgstr_longlong(local_memory_size));

        SET_DEVICE_FIELD(device, adapterString, const char *, context->version, vk_properties->deviceName);
        SET_DEVICE_FIELD(device, vendorId, int, context->version, vk_properties->vendorID);
        SET_DEVICE_FIELD(device, deviceId, int, context->version, vk_properties->deviceID);
        if (vk_properties->vendorID == 0x1002)
        {
            SET_DEVICE_FIELD(device, architectureVersion, ArchitectureVersion, context->version, ArchitectureVersion_GCN);
            SET_DEVICE_FIELD(device, asicFamily, AsicFamily, context->version, AsicFamily_GCN4);
        }
        SET_DEVICE_FIELD(device, localMemoryInBytes, ULONG64, context->version, local_memory_size);
        if (!i)
        {
            if (context->version >= AMD_AGS_VERSION_6_0_0)
            {
                // This is a bitfield now... Nice...
                struct AGSDeviceInfo_600 *device_600 = (struct AGSDeviceInfo_600 *)device;
                device_600->isPrimaryDevice = 1;
            }
            else
            {
                SET_DEVICE_FIELD(device, isPrimaryDevice, int, context->version, 1);
            }   
        }

        if (context->version >= AMD_AGS_VERSION_6_0_0)
        {
            init_device_displays_600(vk_properties->deviceName,
                    GET_DEVICE_FIELD_ADDR(device, displays, AGSDisplayInfo_600 *, context->version),
                    GET_DEVICE_FIELD_ADDR(device, numDisplays, int, context->version));
        }
        else
        {
            init_device_displays_511(vk_properties->deviceName,
                    GET_DEVICE_FIELD_ADDR(device, displays, AGSDisplayInfo_511 *, context->version),
                    GET_DEVICE_FIELD_ADDR(device, numDisplays, int, context->version));
        }

        device += amd_ags_info[context->version].device_size;
    }

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsInit(AGSContext **context, const AGSConfiguration *config, AGSGPUInfo_511 *gpu_info)
{
    struct AGSContext *object;
    AGSReturnCode ret;

    TRACE("context %p, config %p, gpu_info %p.\n", context, config, gpu_info);

    if (!context || !gpu_info)
        return AGS_INVALID_ARGS;

    if (config)
        FIXME("Ignoring config %p.\n", config);

    if (!(object = heap_alloc(sizeof(*object))))
        return AGS_OUT_OF_MEMORY;

    if ((ret = init_ags_context(object)) != AGS_SUCCESS)
    {
        heap_free(object);
        return ret;
    }

    memset(gpu_info, 0, sizeof(*gpu_info));
    gpu_info->agsVersionMajor = amd_ags_info[object->version].major;
    gpu_info->agsVersionMinor = amd_ags_info[object->version].minor;
    gpu_info->agsVersionPatch = amd_ags_info[object->version].patch;
    gpu_info->driverVersion = "21.30.25.05-211005a-372402E-RadeonSoftware";
    gpu_info->radeonSoftwareVersion  = "21.10.2";
    gpu_info->numDevices = object->device_count;
    gpu_info->devices = object->devices;

    TRACE("Created context %p.\n", object);

    *context = object;

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsInitialize(int ags_version, const AGSConfiguration *config, AGSContext **context, AGSGPUInfo_600 *gpu_info)
{
    struct AGSContext *object;
    AGSReturnCode ret;

    TRACE("ags_verison %d, context %p, config %p, gpu_info %p.\n", ags_version, context, config, gpu_info);

    if (!context || !gpu_info)
        return AGS_INVALID_ARGS;

    if (config)
        FIXME("Ignoring config %p.\n", config);

    if (!(object = heap_alloc(sizeof(*object))))
        return AGS_OUT_OF_MEMORY;

    if ((ret = init_ags_context(object)) != AGS_SUCCESS)
    {
        heap_free(object);
        return ret;
    }

    memset(gpu_info, 0, sizeof(*gpu_info));
    gpu_info->driverVersion = "21.30.25.05-211005a-372402E-RadeonSoftware";
    gpu_info->radeonSoftwareVersion  = "21.10.2";
    gpu_info->numDevices = object->device_count;
    gpu_info->devices = object->devices;

    TRACE("Created context %p.\n", object);

    *context = object;

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDeInit(AGSContext *context)
{
    return agsDeInitialize(context);
}

AGSReturnCode WINAPI agsDeInitialize(AGSContext *context)
{
    unsigned int i;
    BYTE *device;

    TRACE("context %p.\n", context);

    if (context)
    {
        heap_free(context->memory_properties);
        heap_free(context->properties);
        device = (BYTE *)context->devices;
        for (i = 0; i < context->device_count; ++i)
        {
            heap_free(*GET_DEVICE_FIELD_ADDR(device, displays, void *, context->version));
            device += amd_ags_info[context->version].device_size;
        }
        heap_free(context->devices);
        heap_free(context);
    }

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsGetCrossfireGPUCount(AGSContext *context, int *gpu_count)
{
    TRACE("context %p gpu_count %p stub!\n", context, gpu_count);

    if (!context || !gpu_count)
        return AGS_INVALID_ARGS;

    *gpu_count = 1;
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_CreateDevice( AGSContext* context,
        const AGSDX11DeviceCreationParams* creation_params, const AGSDX11ExtensionParams* extension_params,
        AGSDX11ReturnedParams* returned_params )
{
    ID3D11DeviceContext *device_context;
    IDXGISwapChain *swapchain = NULL;
    D3D_FEATURE_LEVEL feature_level;
    ID3D11Device *device;
    HRESULT hr;

    TRACE("feature levels %u, pSwapChainDesc %p, app %s, engine %s %#x %#x.\n", creation_params->FeatureLevels,
            creation_params->pSwapChainDesc,
            debugstr_w(extension_params->agsDX11ExtensionParams511.pAppName),
            debugstr_w(extension_params->agsDX11ExtensionParams511.pEngineName),
            extension_params->agsDX11ExtensionParams511.appVersion,
            extension_params->agsDX11ExtensionParams511.engineVersion);

    if (!load_d3d11_functions())
    {
        ERR("Could not load d3d11.dll.\n");
        return AGS_MISSING_D3D_DLL;
    }
    memset( returned_params, 0, amd_ags_info[context->version].dx11_returned_params_size );
    if (creation_params->pSwapChainDesc)
    {
        hr = pD3D11CreateDeviceAndSwapChain(creation_params->pAdapter, creation_params->DriverType,
                creation_params->Software, creation_params->Flags, creation_params->pFeatureLevels,
                creation_params->FeatureLevels, creation_params->SDKVersion, creation_params->pSwapChainDesc,
                &swapchain, &device, &feature_level, &device_context);
    }
    else
    {
        hr = pD3D11CreateDevice(creation_params->pAdapter, creation_params->DriverType,
                creation_params->Software, creation_params->Flags, creation_params->pFeatureLevels,
                creation_params->FeatureLevels, creation_params->SDKVersion,
                &device, &feature_level, &device_context);
    }
    if (FAILED(hr))
    {
        ERR("Device creation failed, hr %#x.\n", hr);
        return AGS_DX_FAILURE;
    }
    if (context->version < AMD_AGS_VERSION_5_2_0)
    {
        AGSDX11ReturnedParams_511 *r = &returned_params->agsDX11ReturnedParams511;
        r->pDevice = device;
        r->pImmediateContext = device_context;
        r->pSwapChain = swapchain;
        r->FeatureLevel = feature_level;
    }
    else if (context->version < AMD_AGS_VERSION_6_0_0)
    {
        AGSDX11ReturnedParams_520 *r = &returned_params->agsDX11ReturnedParams520;
        r->pDevice = device;
        r->pImmediateContext = device_context;
        r->pSwapChain = swapchain;
        r->FeatureLevel = feature_level;
    }
    else
    {
        AGSDX11ReturnedParams_600 *r = &returned_params->agsDX11ReturnedParams600;
        r->pDevice = device;
        r->pImmediateContext = device_context;
        r->pSwapChain = swapchain;
        r->featureLevel = feature_level;
    }

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX12_CreateDevice(AGSContext *context,
        const AGSDX12DeviceCreationParams *creation_params, const AGSDX12ExtensionParams *extension_params,
        AGSDX12ReturnedParams *returned_params)
{
    HRESULT hr;

    TRACE("feature level %#x, app %s, engine %s %#x %#x.\n", creation_params->FeatureLevel, debugstr_w(extension_params->pAppName),
            debugstr_w(extension_params->pEngineName), extension_params->appVersion, extension_params->engineVersion);

    if (!load_d3d12_functions())
    {
        ERR("Could not load d3d12.dll.\n");
        return AGS_MISSING_D3D_DLL;
    }

    memset(returned_params, 0, sizeof(*returned_params));
    if (FAILED(hr = pD3D12CreateDevice((IUnknown *)creation_params->pAdapter, creation_params->FeatureLevel,
            &creation_params->iid, (void **)&returned_params->pDevice)))
    {
        ERR("D3D12CreateDevice failed, hr %#x.\n", hr);
        return AGS_DX_FAILURE;
    }

    TRACE("Created d3d12 device %p.\n", returned_params->pDevice);

    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX12_DestroyDevice(AGSContext* context, ID3D12Device* device, unsigned int* device_refs)
{
    ULONG ref_count;

    if (!device)
        return AGS_SUCCESS;

    ref_count = ID3D12Device_Release(device);
    if (device_refs)
        *device_refs = (unsigned int)ref_count;

    return AGS_SUCCESS;
}

AGSDriverVersionResult WINAPI agsCheckDriverVersion(const char* version_reported, unsigned int version_required)
{
    FIXME("version_reported %s, version_required %d semi-stub.\n", debugstr_a(version_reported), version_required);

    return AGS_SOFTWAREVERSIONCHECK_OK;
}

int WINAPI agsGetVersionNumber(void)
{
    enum amd_ags_version version = determine_ags_version();

    TRACE("version %d.\n", version);

    return AGS_MAKE_VERSION(amd_ags_info[version].major, amd_ags_info[version].minor, amd_ags_info[version].patch);
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_Init( AGSContext* context, ID3D11Device* device, unsigned int uavSlot, unsigned int* extensionsSupported )
{
    FIXME("context %p, device %p, uavSlot %u, extensionsSupported %p stub.\n", context, device, uavSlot, extensionsSupported);

    *extensionsSupported = 0;
    return AGS_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    TRACE("%p, %u, %p.\n", instance, reason, reserved);

    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(instance);
            break;
    }

    return TRUE;
}

#ifdef __x86_64__
AGSReturnCode WINAPI agsDriverExtensionsDX11_SetDepthBounds(AGSContext* context, bool enabled,
        float minDepth, float maxDepth )
{
    static int once;

    if (!once++)
        FIXME("context %p, enabled %#x, minDepth %f, maxDepth %f stub.\n", context, enabled, minDepth, maxDepth);
    return AGS_EXTENSION_NOT_SUPPORTED;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_SetDepthBounds_530(AGSContext* context,
        ID3D11DeviceContext* dxContext, bool enabled, float minDepth, float maxDepth )
{
    static int once;

    if (!once++)
        FIXME("context %p, enabled %#x, minDepth %f, maxDepth %f stub.\n", context, enabled, minDepth, maxDepth);
    return AGS_EXTENSION_NOT_SUPPORTED;
}

__ASM_GLOBAL_FUNC( DX11_SetDepthBounds_impl,
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $3,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp " __ASM_NAME("agsDriverExtensionsDX11_SetDepthBounds") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_SetDepthBounds_530") )

AGSReturnCode WINAPI agsDriverExtensionsDX11_DestroyDevice_520(AGSContext *context, ID3D11Device* device,
        unsigned int *device_ref, ID3D11DeviceContext *device_context,
        unsigned int *context_ref)
{
    ULONG ref;

    TRACE("context %p, device %p, device_ref %p, device_context %p, context_ref %p.\n",
            context, device, device_ref, device_context, context_ref);

    if (!device)
        return AGS_SUCCESS;

    ref = ID3D11Device_Release(device);
    if (device_ref)
        *device_ref = ref;

    if (!device_context)
        return AGS_SUCCESS;

    ref = ID3D11DeviceContext_Release(device_context);
    if (context_ref)
        *context_ref = ref;
    return AGS_SUCCESS;
}

AGSReturnCode WINAPI agsDriverExtensionsDX11_DestroyDevice_511(AGSContext *context, ID3D11Device *device,
        unsigned int *references )
{
    TRACE("context %p, device %p, references %p.\n", context, device, references);

    return agsDriverExtensionsDX11_DestroyDevice_520(context, device, references, NULL, NULL);
}
__ASM_GLOBAL_FUNC( agsDriverExtensionsDX11_DestroyDevice,
                   "mov (%rcx),%eax\n\t" /* version */
                   "cmp $1,%eax\n\t"
                   "jge 1f\n\t"
                   "jmp "     __ASM_NAME("agsDriverExtensionsDX11_DestroyDevice_511") "\n\t"
                   "1:\tjmp " __ASM_NAME("agsDriverExtensionsDX11_DestroyDevice_520") )
#endif
