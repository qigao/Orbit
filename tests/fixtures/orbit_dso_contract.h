#ifndef ORBIT_DSO_PROVIDER_CONTRACT_H
#define ORBIT_DSO_PROVIDER_CONTRACT_H

#include <salts/component_provider_abi.h>

/* Shared strict FR FunctionAbi, not a legacy R0 reflection-only interface.
 * Native CMeta type descriptors are compared semantically across two DSOs.
 */
#define ORBIT_DSO_PROVIDER_EXPORT_ID "orbit.test.component.provider"
#define ORBIT_DSO_VALUE_METHODS(X, I) \
    X(I, FR0, int, get, value, &cmeta_type_int, CMETA_ABI_SCALAR, CMETA_RESULT_VALUE)

CMETA_INTERFACE(orbit_dso_value, ORBIT_DSO_VALUE_METHODS);
CMETA_OBJECT_INTERFACE_ADAPTER(orbit_dso_value);

#endif /* ORBIT_DSO_PROVIDER_CONTRACT_H */
