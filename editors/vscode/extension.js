const { workspace } = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

function activate() {
  const command = workspace.getConfiguration("ziman").get("serverPath") || "ziman";
  client = new LanguageClient(
    "ziman",
    "Ziman",
    { command, args: ["--lsp"] },
    { documentSelector: [{ scheme: "file", language: "ziman" }] }
  );
  client.start();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
