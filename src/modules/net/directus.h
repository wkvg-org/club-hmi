/*
 * Copyright (c) 2026 club-display
 * SPDX-License-Identifier: Apache-2.0
 *
 * Directus CMS backend, ported from the Arduino bar-dashboard
 * (directus.cpp): fetches the day's acts over HTTPS and feeds them to
 * the overview tab. TLS verification is disabled (the reference uses
 * setInsecure()), see PLAN.md.
 */

#ifndef DIRECTUS_H_
#define DIRECTUS_H_

#include <stdbool.h>

/* Downloads and applies the acts; call once after the network is up. */
bool directus_download_acts(void);

/* Re-download on a timer in the service thread. */
void directus_service(void);

#endif /* DIRECTUS_H_ */
