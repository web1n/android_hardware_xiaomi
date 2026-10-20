/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <vector>

// Calls must be serialized with the effect stream cache.
std::vector<int8_t> renderAacPrimitive(uint32_t primitiveId);
