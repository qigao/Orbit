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

cmeta_component(OrbitDsoValueComponent,
    cmeta_provides(orbit_dso_value));

static int provider_value = ORBIT_DSO_PROVIDER_VALUE;
static int provider_get(void *self) { return *(int *)self; }
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
        out, context, &cmeta_data_int, NULL);
}

static const salts_configurator_provider_binding binding = {
    sizeof(salts_configurator_provider_binding),
    SALTS_CONFIGURATOR_PROVIDER_BINDING_ABI_VERSION,
    cmeta_component_meta(OrbitDsoValueComponent),
    &provider_value, &interfaces,
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
