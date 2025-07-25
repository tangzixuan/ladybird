/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/String.h>
#include <AK/Vector.h>

namespace WebView {

void platform_init(Optional<ByteString> ladybird_binary_path = {});
void copy_default_config_files(StringView config_path);
ErrorOr<Vector<ByteString>> get_paths_for_helper_process(StringView process_name);

extern ByteString s_ladybird_resource_root;
Optional<ByteString const&> mach_server_name();
void set_mach_server_name(ByteString name);

ErrorOr<void> handle_attached_debugger();

}
