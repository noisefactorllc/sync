// A loadable Mach-O that carries no Objective-C classes, shaped as a flat
// framework. It stands in for the placeholder Syphon.framework that shipped
// inside Sync.app: NSBundle loads it without complaint, and
// NSClassFromString(@"SyphonMetalServer") still answers nil.
int sync_syphon_stub_marker(void) { return 1; }
