/*
 * messaging.hpp
 *
 * Umbrella header: the complete portable API surface of xmMessaging
 * (xmotion::messaging) — R2: Domain, Advertise/Subscribe, Publish/Loan/
 * take, Client/Server, the five QoS knobs, and the status enums; nothing
 * else. Applications include this one header; components (xmNavigation,
 * xmDriver) never link this library at all (ADR 0005/0006).
 *
 * Copyright (c) 2026 Ruixiang Du (rdu)
 */

#pragma once

#include "xmmessaging/domain.hpp"
#include "xmmessaging/endpoints.hpp"
#include "xmmessaging/payload_traits.hpp"
#include "xmmessaging/qos.hpp"
#include "xmmessaging/sample.hpp"
#include "xmmessaging/status.hpp"
