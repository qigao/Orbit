#if !defined(ORBIT_DSO_PROVIDER_INDEX)
#error ORBIT_DSO_PROVIDER_INDEX is required
#elif ORBIT_DSO_PROVIDER_INDEX == 1
#define ORBIT_DSO_MODULE_ID "orbit.test.provider.n"
#define ORBIT_DSO_PROVIDER_VALUE 37
#elif ORBIT_DSO_PROVIDER_INDEX == 2
#define ORBIT_DSO_MODULE_ID "orbit.test.provider.next"
#define ORBIT_DSO_PROVIDER_VALUE 83
#else
#error unsupported Orbit DSO generation
#endif

#include "orbit_dso_contract.h"
#include <salts/plugin_decl.h>
#include <cmeta/data_reflect.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * M0k.2n: this REAL original Plugin DSO authors the canonical owning
 * CMeta VALUE metadata used in the negative teardown test. The data
 * provider and all of its function pointers live IN THIS MODULE, not in
 * the Orbit host executable. The original strict-FR0 Interface and
 * ServiceHost Provider export ABI stay exactly unchanged.
 *
 * Generation N intentionally violates BufferOps-v2's no-fail semantic-zero
 * postcondition during restore_zero. N+1 provides the same descriptor shape
 * with a correct cleanup. Neither descriptor is admitted to an ordinary
 * published consumer; only the separate quarantined native CTest uses it.
 */
typedef struct orbit_dso_owned_bytes {
    unsigned char *bytes;
    size_t count;
    bool poisoned;
} orbit_dso_owned_bytes;

static bool dso_owned_is_zero(const void *storage) {
    const orbit_dso_owned_bytes *v = (const orbit_dso_owned_bytes *)storage;
    return v && !v->poisoned && v->bytes == NULL && v->count == 0u;
}
static cmeta_status dso_owned_init_zero(void *storage) {
    if (!storage) return CMETA_INVALID_ARGUMENT;
    *(orbit_dso_owned_bytes *)storage = (orbit_dso_owned_bytes){0};
    return CMETA_OK;
}
static void dso_owned_restore_zero(void *storage) {
    orbit_dso_owned_bytes *v = (orbit_dso_owned_bytes *)storage;
    if (!v) return;
    free(v->bytes);
    *v = (orbit_dso_owned_bytes){0};
#if ORBIT_DSO_PROVIDER_INDEX == 1
    /* Deliberately broken DSO-authored no-fail cleanup: the original
     * CMeta facade must detect non-zero and preserve the full Port/Scope.
     */
    v->poisoned = true;
#endif
}
static cmeta_status dso_owned_assign(
    void *storage, const unsigned char *data, size_t size, size_t max_bytes) {
    orbit_dso_owned_bytes *v = (orbit_dso_owned_bytes *)storage;
    unsigned char *copy;
    if (!v || (size != 0u && !data) || !dso_owned_is_zero(v))
        return CMETA_INVALID_ARGUMENT;
    if (size > max_bytes) return CMETA_CAPACITY_EXCEEDED;
    if (size == 0u) return CMETA_OK;
    copy = (unsigned char *)malloc(size);
    if (!copy) return CMETA_OUT_OF_MEMORY;
    memcpy(copy, data, size);
    v->bytes = copy;
    v->count = size;
    return CMETA_OK;
}
static cmeta_status dso_owned_read(
    const void *storage, const unsigned char **bytes, size_t *size) {
    const orbit_dso_owned_bytes *v =
        (const orbit_dso_owned_bytes *)storage;
    if (!v || !bytes || !size || (v->count != 0u && !v->bytes))
        return CMETA_INVALID_ARGUMENT;
    *bytes = v->bytes;
    *size = v->count;
    return CMETA_OK;
}
static void dso_owned_move(void *dest, void *source) {
    orbit_dso_owned_bytes *out = (orbit_dso_owned_bytes *)dest;
    orbit_dso_owned_bytes *in = (orbit_dso_owned_bytes *)source;
    if (!out || !in || out == in || !dso_owned_is_zero(out))
        abort(); /* a declared no-fail provider contract was violated */
    *out = *in;
    *in = (orbit_dso_owned_bytes){0};
}

static const cmeta_type_identity dso_bytes_identity =
    CMETA_TYPE_ID_ATOM_INIT("orbit.test.dso.provider.owned.bytes");
static const cmeta_type_desc dso_bytes_type = {
    "orbit_dso_owned_bytes", sizeof(orbit_dso_owned_bytes),
    CMETA_ALIGNOF(orbit_dso_owned_bytes), CMETA_T_OBJECT, NULL, NULL,
    &dso_bytes_identity
};
static const cmeta_data_buffer_shape dso_bytes_shape = {
    CMETA_DATA_BUFFER_OWNED
};
static const cmeta_data_buffer_ops dso_bytes_ops = {
    sizeof(cmeta_data_buffer_ops), CMETA_DATA_BUFFER_OPS_ABI_VERSION,
    &dso_bytes_type, CMETA_DATA_BUFFER_OWNED,
    dso_owned_is_zero, dso_owned_assign, dso_owned_restore_zero,
    dso_owned_read, dso_owned_init_zero, dso_owned_move
};
static const cmeta_data_desc dso_bytes_desc = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orbit.test.dso.provider.owned.bytes",
    .display_name = "orbit_dso_owned_bytes",
    .kind = CMETA_DATA_BYTES,
    .storage_type = &dso_bytes_type,
    .shape = &dso_bytes_shape,
    .buffer_ops = &dso_bytes_ops
};

typedef struct orbit_dso_provider_state {
    int value;                   /* same original strict-FR0 getter identity */
    orbit_dso_owned_bytes cleanup;
} orbit_dso_provider_state;

/* One complete CMeta VALUE schema, provided BY the original Component DSO.
 * The Scope's canonical ObjectRef.data exposes this metadata without a
 * second Provider Interface, Plugin export or native ABI.
 */
cmeta_reflect_value(
    orbit_dso_provider_state, "orbit.test.dso.provider.State",
    cmeta_data_field(int, value, &cmeta_data_int, &cmeta_type_int)
    cmeta_data_field(orbit_dso_owned_bytes, cleanup,
                     &dso_bytes_desc, &dso_bytes_type)
);

cmeta_component(OrbitDsoValueComponent,
    cmeta_provides(orbit_dso_value));

static orbit_dso_provider_state provider_state = {
    ORBIT_DSO_PROVIDER_VALUE, {0}
};
static int provider_get(void *self) {
    const orbit_dso_provider_state *state =
        (const orbit_dso_provider_state *)self;
    return state->value;
}
CMETA_IMPLEMENTS(orbit_dso_value, orbit_dso_value_impl, 0u,
    .get = provider_get);

static cmeta_status project(
    void *user, const cmeta_object_ref *object,
    const cmeta_interface_desc *expected, cmeta_interface_projection *out) {
    (void)user;
    if (object == NULL || expected == NULL || out == NULL)
        return CMETA_INVALID_ARGUMENT;
    if (!cmeta_interface_desc_equal(
            expected, orbit_dso_value_interface()))
        return CMETA_TRAIT_MISSING;
    out->size = sizeof(*out);
    out->interface = orbit_dso_value_interface();
    out->self = object->object;
    out->dispatch = &orbit_dso_value_impl_vtable;
    return CMETA_OK;
}
static const cmeta_object_interface_provider interfaces = {
    sizeof(cmeta_object_interface_provider), NULL, project
};

static cmeta_status SALTS_CONFIGURATOR_CALL create_provider(
    void *context, const cmeta_data_desc *config_type,
    const void *config_value, const salts_configurator_dependency *deps,
    size_t deps_count, cmeta_object_ref *out) {
    (void)deps;
    if (context == NULL || out == NULL || config_type != NULL ||
        config_value != NULL || deps_count != 0u)
        return CMETA_INVALID_ARGUMENT;
    return cmeta_object_borrow(
        out, context, cmeta_reflected_data(orbit_dso_provider_state), NULL);
}

static const salts_configurator_provider_binding binding = {
    sizeof(salts_configurator_provider_binding),
    SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
    cmeta_component_meta(OrbitDsoValueComponent),
    &provider_state, &interfaces,
    create_provider, NULL, NULL
};

static const salts_configurator_provider_binding *get_binding(void *self) {
    return (const salts_configurator_provider_binding *)self;
}
CMETA_IMPLEMENTS(
    salts_service_host_provider, orbit_dso_binding_impl, 0u,
    .get_binding = get_binding);
static salts_service_host_provider service_provider = {
    (void *)&binding, &orbit_dso_binding_impl_vtable
};

#define ORBIT_DSO_EXPORTS(X) \
    X(interface, (salts_service_host_provider, &service_provider), \
      ORBIT_DSO_PROVIDER_EXPORT_ID, SALTS_SERVICE_HOST_PROVIDER_CONTRACT_ID, \
      SALTS_SERVICE_HOST_PROVIDER_CONTRACT_VERSION, 0)

/* Original Salts Plugin manifest/registry. No Orbit loader or second ABI. */
CMETA_PLUGIN_DECLARE(
    orbit_dso_provider, ORBIT_DSO_MODULE_ID, (1,0,0),
    ORBIT_DSO_EXPORTS, CMETA_PLUGIN_PASSIVE());
