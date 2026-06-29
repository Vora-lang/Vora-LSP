/**
 * Vora DAP server — entry point.
 *
 * Creates a DapServer and enters the main stdio loop.
 * The debug adapter communicates with VS Code via DAP over stdin/stdout.
 */
#include "dap_server.h"

int main() {
    vora::dap::DapServer server;
    server.run();
    return 0;
}
