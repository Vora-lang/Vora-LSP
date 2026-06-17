/**
 * Vora LSP server — entry point.
 *
 * Creates an LspServer and enters the main stdio loop.
 * All configuration comes from the client via LSP protocol messages.
 */
#include "lsp_server.h"

int main() {
    vora::lsp::LspServer server;
    server.run();
    return 0;
}
