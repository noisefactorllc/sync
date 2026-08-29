// A loadable Mach-O that carries no Objective-C classes, shaped as a flat
// framework. It stands in for a Syphon.framework that is present and useless:
// NSBundle loads it without complaint, and NSClassFromString(@"SyphonMetalServer")
// still answers nil. A locally packaged Sync.app carried one of these; nothing
// here claims a release did.
int sync_syphon_stub_marker(void) { return 1; }

// Keep an adversarial near-match in the symbol table so packaging checks must
// recognize the complete Objective-C class symbol, not a string prefix.
extern "C" int sync_syphon_near_name(void)
    __asm__("_OBJC_CLASS_$_SyphonMetalServerFake");
extern "C" int sync_syphon_near_name(void) { return 1; }
