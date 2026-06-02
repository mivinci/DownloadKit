/*
 * Copyright 2025 The libx++ Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * builder.cpp - Runtime builder.
 */

#include <xpp/runtime/builder.h>

#include <xpp/runtime/runtime.h>

namespace xpp {
namespace runtime {

Box<Runtime> Builder::build() const {
  return Box<Runtime>::from_raw(new Runtime(*this));
}

} // namespace runtime
} // namespace xpp
