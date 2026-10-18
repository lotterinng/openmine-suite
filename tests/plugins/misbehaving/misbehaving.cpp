// SPDX-License-Identifier: Apache-2.0
// Test plugin exercising the host's protections.
#include "openmine_plugin.h"

#include <cstring>
#include <string>

namespace {
const OpenMineHostV1* H = nullptr;
int leaveEditOpen(OmHostContext* ctx, void*, const char*, OmString*) {
    H->begin_edit(ctx, "Never finished");
    OmString k{};
    H->add_geometry(ctx, R"({"type":"Point","name":"orphan","layer":"0","vertices":[[1,2,3]]})", &k);
    H->free_string(&k);
    return OM_OK;
}
int invalidDocument(OmHostContext* ctx, void*, const char*, OmString*) {
    OmString t{};
    H->template_document(ctx, "SurveyControlPoint", &t); // name left empty → invalid
    const std::string doc(t.data, t.size);
    H->free_string(&t);
    return H->put_document(ctx, "SurveyControlPoint", doc.c_str(), nullptr);
}
int lateRegistration(OmHostContext* ctx, void*, const char*, OmString*) { return H->register_command(ctx, "late.cmd", "", lateRegistration, nullptr); }
int throws(OmHostContext*, void*, const char*, OmString*) { throw 42; }
} // namespace

extern "C" OPENMINE_PLUGIN_EXPORT int openmine_plugin_init_v1(const OpenMineHostV1* host, OmHostContext* ctx, OpenMinePluginInfoV1* info) {
    H = host;
    info->id = "org.openmine.tests.misbehaving";
    info->version = "0.0.1";
    host->register_command(ctx, "bad.open-edit", "", leaveEditOpen, nullptr);
    host->register_command(ctx, "bad.invalid", "", invalidDocument, nullptr);
    host->register_command(ctx, "bad.late", "", lateRegistration, nullptr);
    host->register_command(ctx, "bad.throws", "", throws, nullptr);
    // A duplicate registration must be refused (and the plugin may carry on).
    if (host->register_command(ctx, "bad.open-edit", "", leaveEditOpen, nullptr) == OM_OK) return OM_ERROR;
    return OM_OK;
}
