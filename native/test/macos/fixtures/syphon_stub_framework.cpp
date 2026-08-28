// A loadable Mach-O that carries no Objective-C classes, shaped as a flat
// framework. It stands in for a Syphon.framework that is present and useless:
// NSBundle loads it without complaint, and NSClassFromString(@"SyphonMetalServer")
// still answers nil. A locally packaged Sync.app carried one of these; nothing
// here claims a release did.
int sync_syphon_stub_marker(void) { return 1; }
