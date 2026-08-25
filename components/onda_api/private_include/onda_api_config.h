#ifndef ONDA_API_CONFIG_H
#define ONDA_API_CONFIG_H

/*
 * The local header is deliberately ignored by Git. Without it the component
 * remains buildable but reports a configuration error when the check is run.
 */
#ifdef ONDA_API_LOCAL_CONFIG_AVAILABLE
#include "onda_api_config_local.h"
#define ONDA_API_BASE_URL ONDA_API_LOCAL_BASE_URL
#define ONDA_API_DEVICE_TOKEN ONDA_API_LOCAL_DEVICE_TOKEN
#else
#define ONDA_API_BASE_URL ""
#define ONDA_API_DEVICE_TOKEN ""
#endif

#endif
