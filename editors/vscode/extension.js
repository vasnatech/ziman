const vscode = require("vscode");
const { workspace, window, commands } = vscode;
const { LanguageClient } = require("vscode-languageclient/node");

let client;
let terminal;

function serverCommand() {
  return workspace.getConfiguration("ziman").get("serverPath") || "ziman";
}

function startClient() {
  client = new LanguageClient(
    "ziman",
    "Ziman",
    { command: serverCommand(), args: ["--lsp"] },
    { documentSelector: [{ scheme: "file", language: "ziman" }] }
  );
  return client.start();
}

async function restartClient() {
  if (client) {
    try {
      await client.stop();
    } catch (e) {
      // a server that never started has nothing to stop
    }
  }
  return startClient();
}

function quote(text) {
  return `"${text.replace(/(["\\$`])/g, "\\$1")}"`;
}

// Runs `ziman [flags] file` in a terminal of its own, first saving the file.
async function runInTerminal(flags) {
  const editor = window.activeTextEditor;
  if (!editor || editor.document.languageId !== "ziman") {
    window.showInformationMessage("Open a .zm file first.");
    return;
  }
  await editor.document.save();
  if (!terminal || terminal.exitStatus !== undefined) {
    terminal = window.createTerminal("Ziman");
  }
  terminal.show(true);
  const cwd = workspace.getWorkspaceFolder(editor.document.uri)?.uri.fsPath;
  const cd = cwd ? `cd ${quote(cwd)} && ` : "";
  terminal.sendText(`${cd}${quote(serverCommand())} ${flags}${quote(editor.document.uri.fsPath)}`);
}

// The nearest main.zm above the current file: the program to run when a library file is open.
async function runProgram() {
  const editor = window.activeTextEditor;
  if (!editor || editor.document.languageId !== "ziman") {
    window.showInformationMessage("Open a .zm file first.");
    return;
  }
  const dir = vscode.Uri.joinPath(editor.document.uri, "..");
  const found = await workspace.findFiles(new vscode.RelativePattern(dir, "main.zm"), null, 1);
  if (found.length === 0) {
    return runInTerminal("");
  }
  await editor.document.save();
  if (!terminal || terminal.exitStatus !== undefined) {
    terminal = window.createTerminal("Ziman");
  }
  terminal.show(true);
  terminal.sendText(`cd ${quote(dir.fsPath)} && ${quote(serverCommand())} main.zm`);
}

function activate(context) {
  startClient().catch((e) => {
    window.showErrorMessage(
      `Ziman: could not start the language server (${serverCommand()} --lsp). Set "ziman.serverPath" to the ziman executable. ${e.message || e}`
    );
  });

  context.subscriptions.push(
    commands.registerCommand("ziman.runFile", () => runInTerminal("")),
    commands.registerCommand("ziman.runProgram", runProgram),
    commands.registerCommand("ziman.checkFile", () => runInTerminal("--check ")),
    commands.registerCommand("ziman.restartServer", async () => {
      await restartClient();
      window.showInformationMessage("Ziman language server restarted.");
    }),
    window.onDidCloseTerminal((t) => {
      if (t === terminal) terminal = undefined;
    }),
    workspace.onDidChangeConfiguration(async (e) => {
      if (e.affectsConfiguration("ziman.serverPath")) {
        const choice = await window.showInformationMessage("Ziman: the server path changed.", "Restart server");
        if (choice) await restartClient();
      }
    })
  );
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
