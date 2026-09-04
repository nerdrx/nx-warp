/* nxvc_vk.h - C ABI for the NX Warp shared Vulkan runtime (nxvc_vk_common).
 *
 * This is the seam that WiVRn NX / Monado link against.  It exposes:
 *
 *   1. the capability probe of docs/PAPER.md 3.7 (Full / Lite / hybrid-only /
 *      unsupported) over a VkPhysicalDevice, with a JSON serialisation, and
 *   2. context creation -- either the library creates its own VkInstance /
 *      VkDevice / compute queue, or it *adopts* a device the host already
 *      owns (WiVRn's server runs on Monado's VkDevice; the Android client
 *      runs on the client's device).
 *
 * The heavy lifting for the encoder and decoder lives in the C++20 headers
 * (nxvc/vk/vk_common.hpp).  This header stays C99, handle-based and stable.
 *
 * Threading: an nxvc_vk_context is not internally synchronised.  Probe calls
 * are pure and reentrant.
 */
#ifndef NXVC_VK_NXVC_VK_H
#define NXVC_VK_NXVC_VK_H

#include <stdint.h>
#include <stddef.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXVC_VK_ABI_VERSION 1

/* The normative cluster width of 3.2.6: every cross-lane exchange wider than
 * this goes through LDS.  A device whose subgroups can be narrower than this
 * cannot run the pure-compute path. */
#define NXVC_VK_CLUSTER_WIDTH 8

/* ------------------------------------------------------------------ status */
typedef enum nxvc_vk_status {
    NXVC_VK_OK = 0,
    NXVC_VK_ERR_ARG = -1,          /* bad argument from the caller           */
    NXVC_VK_ERR_UNSUPPORTED = -2,  /* device fails the 3.7 capability rules  */
    NXVC_VK_ERR_VULKAN = -3,       /* a VkResult came back non-success       */
    NXVC_VK_ERR_NOMEM = -4,        /* host or device allocation failed       */
    NXVC_VK_ERR_NO_DEVICE = -5,    /* no physical device matched             */
    NXVC_VK_ERR_INTERNAL = -6
} nxvc_vk_status;

const char* nxvc_vk_status_string(nxvc_vk_status s);

/* ----------------------------------------------------------------- profile */
/* docs/PAPER.md 3.7.  The ladder is strictly ordered: a FULL device can do
 * everything a LITE device can, and so on down. */
typedef enum nxvc_vk_profile {
    NXVC_VK_PROFILE_UNSUPPORTED = 0, /* not even the hybrid Pass C path      */
    NXVC_VK_PROFILE_HYBRID_ONLY = 1, /* hardware base layer + Pass C only    */
    NXVC_VK_PROFILE_LITE = 2,        /* pure compute, reduced resources      */
    NXVC_VK_PROFILE_FULL = 3         /* pure compute at full tile geometry   */
} nxvc_vk_profile;

const char* nxvc_vk_profile_string(nxvc_vk_profile p);

/* Named capability bits, so a caller can report *why* without string diffing.
 * The REQ_ group is what 3.7 demands of the pure-compute path. */
typedef enum nxvc_vk_cap_bit {
    NXVC_VK_CAP_API_1_1              = 1u << 0,
    NXVC_VK_CAP_COMPUTE_QUEUE        = 1u << 1,
    NXVC_VK_CAP_SUBGROUP_BASIC       = 1u << 2,
    NXVC_VK_CAP_SUBGROUP_BALLOT      = 1u << 3,
    NXVC_VK_CAP_SUBGROUP_SHUFFLE     = 1u << 4,
    NXVC_VK_CAP_SUBGROUP_ARITHMETIC  = 1u << 5,
    NXVC_VK_CAP_SUBGROUP_CLUSTERED   = 1u << 6,  /* advisory, see note below */
    NXVC_VK_CAP_SUBGROUP_VOTE        = 1u << 7,
    NXVC_VK_CAP_SUBGROUP_WIDTH_8     = 1u << 8,  /* subgroups are >= 8 lanes */
    NXVC_VK_CAP_STORAGE_16BIT        = 1u << 9,
    NXVC_VK_CAP_TIMELINE_SEMAPHORE   = 1u << 10,
    NXVC_VK_CAP_TIMESTAMP_QUERY      = 1u << 11,
    NXVC_VK_CAP_SHARED_MEMORY_32K    = 1u << 12,
    NXVC_VK_CAP_WORKGROUP_256        = 1u << 13,
    /* Optional / platform capabilities, never fatal. */
    NXVC_VK_CAP_SUBGROUP_SIZE_CONTROL= 1u << 14,
    NXVC_VK_CAP_SHADER_INT16         = 1u << 15,
    NXVC_VK_CAP_SHADER_INT64         = 1u << 16,
    NXVC_VK_CAP_HOST_CACHED_HEAP     = 1u << 17, /* 3.6 send ring, encoder   */
    NXVC_VK_CAP_EXTERNAL_MEMORY_FD   = 1u << 18,
    NXVC_VK_CAP_EXTERNAL_SEMAPHORE_FD= 1u << 19,
    NXVC_VK_CAP_EXTERNAL_MEMORY_WIN32= 1u << 20,
    NXVC_VK_CAP_EXTERNAL_SEM_WIN32   = 1u << 21,
    NXVC_VK_CAP_ANDROID_HW_BUFFER    = 1u << 22,
    NXVC_VK_CAP_YCBCR_CONVERSION     = 1u << 23,
    NXVC_VK_CAP_PIPELINE_EXEC_PROPS  = 1u << 24
} nxvc_vk_cap_bit;

/* Mask of the bits a device must have to run the normative pure-compute path.
 *
 * NXVC_VK_CAP_SUBGROUP_CLUSTERED is deliberately *not* in here.  3.2.6 forbids
 * `subgroupClustered*` in normative shaders (Adreno's proprietary compiler
 * handles them badly) and emulates clusters of 8 with ballot + masks derived
 * from `gl_SubgroupInvocationID & ~7`.  The probe still reports the bit,
 * because the subgroup-semantics conformance kernel uses the real clustered
 * ops as the oracle it checks the emulation against. */
#define NXVC_VK_CAPS_REQUIRED_PURE                                            \
    (NXVC_VK_CAP_API_1_1 | NXVC_VK_CAP_COMPUTE_QUEUE |                        \
     NXVC_VK_CAP_SUBGROUP_BASIC | NXVC_VK_CAP_SUBGROUP_BALLOT |               \
     NXVC_VK_CAP_SUBGROUP_SHUFFLE | NXVC_VK_CAP_SUBGROUP_ARITHMETIC |         \
     NXVC_VK_CAP_SUBGROUP_VOTE | NXVC_VK_CAP_SUBGROUP_WIDTH_8 |               \
     NXVC_VK_CAP_STORAGE_16BIT | NXVC_VK_CAP_TIMELINE_SEMAPHORE |             \
     NXVC_VK_CAP_TIMESTAMP_QUERY)

/* On top of REQUIRED_PURE, these separate FULL from LITE. */
#define NXVC_VK_CAPS_REQUIRED_FULL                                            \
    (NXVC_VK_CAP_SHARED_MEMORY_32K | NXVC_VK_CAP_WORKGROUP_256)

/* The floor for the hybrid path of 3.5: a compute queue and ballot.  */
#define NXVC_VK_CAPS_REQUIRED_HYBRID                                          \
    (NXVC_VK_CAP_API_1_1 | NXVC_VK_CAP_COMPUTE_QUEUE |                        \
     NXVC_VK_CAP_SUBGROUP_BASIC | NXVC_VK_CAP_SUBGROUP_BALLOT)

/* --------------------------------------------------------------- gpu vendor */
typedef enum nxvc_vk_vendor {
    NXVC_VK_VENDOR_UNKNOWN = 0,
    NXVC_VK_VENDOR_AMD,
    NXVC_VK_VENDOR_NVIDIA,
    NXVC_VK_VENDOR_INTEL,
    NXVC_VK_VENDOR_QUALCOMM,   /* Adreno   */
    NXVC_VK_VENDOR_ARM,        /* Mali     */
    NXVC_VK_VENDOR_IMGTEC,
    NXVC_VK_VENDOR_APPLE,      /* MoltenVK */
    NXVC_VK_VENDOR_MESA_SOFTWARE, /* lavapipe */
    NXVC_VK_VENDOR_SWIFTSHADER
} nxvc_vk_vendor;

const char* nxvc_vk_vendor_string(nxvc_vk_vendor v);

/* ------------------------------------------------------------------- probe */
#define NXVC_VK_MAX_NAME 256
#define NXVC_VK_MAX_NOTES 8
#define NXVC_VK_MAX_NOTE_LEN 160

typedef struct nxvc_vk_probe {
    /* identity */
    char     device_name[NXVC_VK_MAX_NAME];
    char     driver_name[NXVC_VK_MAX_NAME];
    char     driver_info[NXVC_VK_MAX_NAME];
    uint32_t api_version;
    uint32_t driver_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t device_type;          /* VkPhysicalDeviceType */
    uint32_t driver_id;            /* VkDriverId, 0 if unknown */
    nxvc_vk_vendor vendor;

    /* subgroup shape -- 3.2.6 / 3.7 */
    uint32_t subgroup_size;        /* the reported native size            */
    uint32_t subgroup_size_min;    /* size-control range, == size if none */
    uint32_t subgroup_size_max;
    uint32_t subgroup_supported_ops;   /* VkSubgroupFeatureFlags          */
    uint32_t subgroup_supported_stages;/* VkShaderStageFlags              */
    uint32_t required_subgroup_size;   /* what we would pin, 0 = leave it */
    uint32_t max_compute_workgroup_subgroups;
    uint32_t quad_operations_in_all_stages;

    /* compute limits */
    uint32_t max_compute_shared_memory_size;
    uint32_t max_compute_workgroup_invocations;
    uint32_t max_compute_workgroup_count[3];
    uint32_t max_compute_workgroup_size[3];
    uint32_t max_storage_buffer_range;
    uint32_t max_push_constants_size;

    /* queues */
    uint32_t compute_queue_family;      /* best pick, UINT32_MAX if none   */
    uint32_t compute_queue_is_dedicated;/* async compute, no graphics bit  */
    uint32_t transfer_queue_family;     /* UINT32_MAX if none distinct     */
    uint32_t queue_family_count;

    /* timing */
    float    timestamp_period_ns;
    uint32_t timestamp_valid_bits;      /* on the chosen compute family    */

    /* memory (3.6: the send ring wants HOST_VISIBLE|COHERENT|CACHED) */
    uint64_t device_local_bytes;
    uint64_t host_cached_bytes;
    uint32_t host_cached_type_index;    /* UINT32_MAX if none              */
    uint32_t device_local_host_visible_type_index; /* the BAR heap, if any */

    /* capabilities */
    uint32_t caps;                      /* nxvc_vk_cap_bit bitmask         */
    uint32_t caps_missing_for_pure;     /* REQUIRED_PURE & ~caps           */
    uint32_t caps_missing_for_full;     /* REQUIRED_FULL & ~caps           */

    /* verdict */
    nxvc_vk_profile profile;
    char     reason[NXVC_VK_MAX_NOTE_LEN];             /* why this profile */
    char     notes[NXVC_VK_MAX_NOTES][NXVC_VK_MAX_NOTE_LEN];
    uint32_t note_count;
} nxvc_vk_probe;

/* Probe a physical device.  `instance` must have been created with at least
 * apiVersion 1.1.  Never fails on a well-formed argument: an unsupported
 * device comes back with profile == UNSUPPORTED and a filled-in reason. */
nxvc_vk_status nxvc_vk_probe_physical_device(VkInstance instance,
                                             VkPhysicalDevice pd,
                                             nxvc_vk_probe* out);

/* Convenience: create a throwaway 1.1 instance, probe every physical device,
 * write up to `capacity` results, and report how many exist.  `out` may be
 * NULL to query the count only. */
nxvc_vk_status nxvc_vk_probe_all(nxvc_vk_probe* out, uint32_t capacity,
                                 uint32_t* out_count);

/* Serialise a probe as JSON.  Returns the number of bytes that would have
 * been written excluding the NUL (snprintf semantics); `buf` may be NULL. */
int nxvc_vk_probe_to_json(const nxvc_vk_probe* p, char* buf, size_t cap);

/* The list of device extensions the library wants enabled for `profile` on
 * this device.  Returns the count; fills up to `capacity` `const char*`.
 * A host that creates its own VkDevice (Monado, WiVRn) calls this and merges
 * the result into its own extension list before vkCreateDevice. */
uint32_t nxvc_vk_required_device_extensions(const nxvc_vk_probe* p,
                                            const char** out,
                                            uint32_t capacity);

/* Same for the features that must be enabled.  The caller passes a chain head
 * it owns; the library fills a fixed set of structs it also owns (static
 * storage per call is *not* used -- the structs live in `chain`, see below).
 *
 * `chain` must point to an nxvc_vk_feature_chain the caller keeps alive until
 * vkCreateDevice returns.  On success `*out_pnext` is the head to splice into
 * VkDeviceCreateInfo::pNext. */
typedef struct nxvc_vk_feature_chain nxvc_vk_feature_chain;
size_t nxvc_vk_feature_chain_size(void);
nxvc_vk_status nxvc_vk_fill_feature_chain(const nxvc_vk_probe* p,
                                          nxvc_vk_feature_chain* chain,
                                          void** out_pnext);

/* ----------------------------------------------------------------- context */
typedef struct nxvc_vk_context nxvc_vk_context;

typedef enum nxvc_vk_context_flags {
    NXVC_VK_CTX_VALIDATION      = 1u << 0, /* enable the Khronos layer      */
    NXVC_VK_CTX_PREFER_DISCRETE = 1u << 1,
    NXVC_VK_CTX_PREFER_SOFTWARE = 1u << 2, /* CI: pick lavapipe/SwiftShader */
    NXVC_VK_CTX_DEDICATED_COMPUTE = 1u << 3, /* async compute queue if any  */
    NXVC_VK_CTX_ALLOW_HYBRID    = 1u << 4  /* accept HYBRID_ONLY devices    */
} nxvc_vk_context_flags;

typedef struct nxvc_vk_context_create_info {
    uint32_t     abi_version;      /* must be NXVC_VK_ABI_VERSION */
    uint32_t     flags;            /* nxvc_vk_context_flags       */
    const char*  app_name;         /* may be NULL                 */
    uint32_t     device_index;     /* UINT32_MAX = auto-select    */
} nxvc_vk_context_create_info;

/* Create everything: instance, physical device selection by the 3.7 rules,
 * logical device, compute queue. */
nxvc_vk_status nxvc_vk_context_create(const nxvc_vk_context_create_info* ci,
                                      nxvc_vk_context** out);

/* Adopt a device the host already owns.  The library never destroys anything
 * it did not create; the host guarantees the handles outlive the context.
 *
 * `queue_family` must be a family with VK_QUEUE_COMPUTE_BIT that `queue` was
 * taken from.  `get_instance_proc_addr` may be NULL to use the linked loader
 * (needed when the host loaded Vulkan through volk or a bundled loader).
 *
 * The probe is run and stored, but adoption does NOT fail on an unsupported
 * profile: the caller may legitimately want the hybrid path.  Check
 * nxvc_vk_context_probe()->profile. */
typedef struct nxvc_vk_adopt_info {
    uint32_t                    abi_version;
    VkInstance                  instance;
    VkPhysicalDevice            physical_device;
    VkDevice                    device;
    VkQueue                     queue;
    uint32_t                    queue_family;
    uint32_t                    api_version;   /* the host's apiVersion   */
    PFN_vkGetInstanceProcAddr   get_instance_proc_addr; /* may be NULL    */
    const char* const*          enabled_device_extensions; /* may be NULL */
    uint32_t                    enabled_device_extension_count;
} nxvc_vk_adopt_info;

nxvc_vk_status nxvc_vk_context_adopt(const nxvc_vk_adopt_info* ai,
                                     nxvc_vk_context** out);

void nxvc_vk_context_destroy(nxvc_vk_context* ctx);

/* Accessors.  All return handles owned by the context (or by the host, for an
 * adopted context); never destroy them. */
VkInstance       nxvc_vk_context_instance(const nxvc_vk_context* ctx);
VkPhysicalDevice nxvc_vk_context_physical_device(const nxvc_vk_context* ctx);
VkDevice         nxvc_vk_context_device(const nxvc_vk_context* ctx);
VkQueue          nxvc_vk_context_queue(const nxvc_vk_context* ctx);
uint32_t         nxvc_vk_context_queue_family(const nxvc_vk_context* ctx);
int              nxvc_vk_context_is_adopted(const nxvc_vk_context* ctx);
const nxvc_vk_probe* nxvc_vk_context_probe(const nxvc_vk_context* ctx);

/* The last human-readable error, valid until the next call on this context.
 * Pass NULL for errors raised before a context existed. */
const char* nxvc_vk_last_error(const nxvc_vk_context* ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NXVC_VK_NXVC_VK_H */
