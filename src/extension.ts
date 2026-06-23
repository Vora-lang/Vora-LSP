/**
 * Vora VS Code Extension — Language Client
 *
 * Launches the Vora LSP server (vora-lsp) as a child process and
 * connects VS Code's language features to it via the Language Server
 * Protocol (LSP) over stdio.
 *
 * Features enabled through the LSP server:
 *   - Diagnostics (parse errors / warnings on open and change)
 *   - Code formatting (document and selection)
 *   - Completion, go-to-definition, hover, document symbols
 *     (return empty results until semantic analysis is implemented)
 */

import * as path from 'path';
import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext): void {
    // ── Server configuration ────────────────────────────────────────────
    const config = vscode.workspace.getConfiguration('vora');
    const serverPath: string = config.get('lsp.serverPath', 'vora-lsp');

    // Resolve relative paths against the extension directory.
    const resolvedPath = path.isAbsolute(serverPath)
        ? serverPath
        : path.join(context.extensionPath, serverPath);

    // ── Verify server binary exists ──────────────────────────────────────
    const fs = await import('fs');
    if (!fs.existsSync(resolvedPath)) {
        void vscode.window.showErrorMessage(
            `Vora: Language server binary not found at "${resolvedPath}". ` +
            'Build the LSP server first, or set "vora.lsp.serverPath" to the correct path.',
        );
        return;
    }

    const serverOptions: ServerOptions = {
        command: resolvedPath,
        args: [],
        transport: TransportKind.stdio,
    };

    // ── Client configuration ────────────────────────────────────────────
    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'vora' }],
        synchronize: {
            // Notify the server about file changes to .va files in the workspace.
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.va'),
        },
        outputChannelName: 'Vora LSP',
        traceOutputChannel: vscode.window.createOutputChannel('Vora LSP Trace'),
    };

    // ── Create and start the client ─────────────────────────────────────
    client = new LanguageClient(
        'vora-lsp',
        'Vora Language Server',
        serverOptions,
        clientOptions,
    );

    // Register commands.
    context.subscriptions.push(
        vscode.commands.registerCommand('vora.restartServer', async () => {
            await restartServer();
        }),
    );

    // Start the client. This launches the server and begins listening.
    client.start().then(() => {
        client?.outputChannel.appendLine('Vora LSP server started');
    }).catch((err: unknown) => {
        void vscode.window.showErrorMessage(
            `Vora: Failed to start language server: ${err}`,
        );
    });

    // Show a status bar item.
    const statusBarItem = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Right,
        100,
    );
    statusBarItem.text = '$(code) Vora';
    statusBarItem.tooltip = 'Vora Language Server';
    statusBarItem.command = 'vora.restartServer';
    statusBarItem.show();
    context.subscriptions.push(statusBarItem);
}

export function deactivate(): Thenable<void> | undefined {
    if (client) {
        return client.stop();
    }
    return undefined;
}

async function restartServer(): Promise<void> {
    if (!client) {
        return;
    }
    try {
        await client.restart();
        void vscode.window.showInformationMessage('Vora LSP server restarted');
    } catch (err: unknown) {
        void vscode.window.showErrorMessage(
            `Vora: Failed to restart language server: ${err}`,
        );
    }
}
