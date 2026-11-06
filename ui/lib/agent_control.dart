// Agent Control panel: start/stop the locked-down HTTP API server
// (bytecode-api) with a generated bearer token, surface the MCP launch command,
// and demonstrate the in-process FFI (keygen) via libbytecode_tools.
//
// This is the GUI side of "wiring" the agentic-control servers in: an operator
// can hand an external agent either the HTTP endpoint + token or the MCP
// command, both of which drive the full bytecode tool surface.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'bytecode_ffi.dart';

class AgentControlPanel extends StatefulWidget {
  final String root;
  final String phpBinary;
  final void Function(String) onLog;

  const AgentControlPanel({
    super.key,
    required this.root,
    required this.phpBinary,
    required this.onLog,
  });

  @override
  State<AgentControlPanel> createState() => _AgentControlPanelState();
}

class _AgentControlPanelState extends State<AgentControlPanel> {
  final _addrController = TextEditingController(text: '127.0.0.1:8787');
  Process? _api;
  String _token = '';
  String _status = 'stopped';
  String _ffiKey = '';
  int _phpId = -1;
  BytecodeFfi? _ffi;
  bool _ffiProbed = false;

  @override
  void dispose() {
    _api?.kill();
    _addrController.dispose();
    super.dispose();
  }

  String _mcpCommand() => '${widget.root}/php/bin/bytecode-mcp';

  String _genToken() {
    final r = Random.secure();
    final bytes = List<int>.generate(24, (_) => r.nextInt(256));
    return base64Url.encode(bytes).replaceAll('=', '');
  }

  BytecodeFfi? _ensureFfi() {
    if (!_ffiProbed) {
      _ffiProbed = true;
      _ffi = BytecodeFfi.open(widget.root);
      if (_ffi != null) {
        try {
          _phpId = _ffi!.detectPhpVersionId();
        } catch (_) {}
      }
    }
    return _ffi;
  }

  void _ffiKeygen() {
    final f = _ensureFfi();
    if (f == null) {
      widget.onLog('ffi: libbytecode_tools not found — build it with '
          '(cd ${widget.root}/rust && cargo build --release)\n');
      return;
    }
    try {
      final key = f.keygenHex();
      setState(() => _ffiKey = key);
      widget.onLog('ffi keygen (in-process): $key\n');
    } catch (e) {
      widget.onLog('ffi keygen failed: $e\n');
    }
  }

  Future<void> _startApi() async {
    if (_api != null) return;
    final token = _genToken();
    final addr = _addrController.text.trim();
    final launcher = '${widget.root}/php/bin/bytecode-api';
    try {
      final proc = await Process.start(
        widget.phpBinary,
        [launcher, addr],
        environment: {
          'BYTECODE_API_TOKEN': token,
          'BYTECODE_ROOT': widget.root,
        },
        workingDirectory: widget.root,
      );
      proc.stdout.transform(utf8.decoder).listen(widget.onLog);
      proc.stderr.transform(utf8.decoder).listen(widget.onLog);
      unawaited(proc.exitCode.then((code) {
        if (mounted) {
          setState(() {
            _status = 'stopped (exit $code)';
            _api = null;
          });
        }
      }));
      setState(() {
        _api = proc;
        _token = token;
        _status = 'running on http://$addr';
      });
      widget.onLog('agent api: started on http://$addr (bearer auth)\n');
    } catch (e) {
      widget.onLog('agent api: failed to start: $e\n');
    }
  }

  Future<void> _stopApi() async {
    final p = _api;
    if (p == null) return;
    p.kill();
    widget.onLog('agent api: stop requested\n');
  }

  void _copy(String label, String value) {
    Clipboard.setData(ClipboardData(text: value));
    widget.onLog('copied $label to clipboard\n');
  }

  String _curlExample() {
    final addr = _addrController.text.trim();
    return "curl -H 'Authorization: Bearer $_token' "
        "-X POST http://$addr/tools/verify "
        "-d '{\"args\":[\"--decrypt-test\",\"<output-dir>\"],"
        "\"env\":{\"BYTECODE_KEY\":\"<64hex>\"}}'";
  }

  @override
  Widget build(BuildContext context) {
    final running = _api != null;
    return Scaffold(
      appBar: AppBar(title: const Text('Agent Control')),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          _section('In-process library (FFI)', [
            Text(_ensureFfi() != null
                ? 'libbytecode_tools loaded${_phpId >= 0 ? " · PHP_VERSION_ID $_phpId" : ""}'
                : 'libbytecode_tools not built (cd rust && cargo build --release)'),
            const SizedBox(height: 8),
            Row(children: [
              FilledButton.icon(
                onPressed: _ffiKeygen,
                icon: const Icon(Icons.vpn_key),
                label: const Text('Generate key (in-process)'),
              ),
              const SizedBox(width: 12),
              if (_ffiKey.isNotEmpty)
                Expanded(
                  child: SelectableText(_ffiKey,
                      style: const TextStyle(fontFamily: 'monospace')),
                ),
            ]),
          ]),
          _section('HTTP API server (bytecode-api)', [
            TextField(
              controller: _addrController,
              enabled: !running,
              decoration: const InputDecoration(
                labelText: 'bind address',
                helperText: 'loopback only unless BYTECODE_API_ALLOW_REMOTE=1',
              ),
            ),
            const SizedBox(height: 8),
            Text('status: $_status'),
            const SizedBox(height: 8),
            Row(children: [
              FilledButton.icon(
                onPressed: running ? null : _startApi,
                icon: const Icon(Icons.play_arrow),
                label: const Text('Start'),
              ),
              const SizedBox(width: 12),
              OutlinedButton.icon(
                onPressed: running ? _stopApi : null,
                icon: const Icon(Icons.stop),
                label: const Text('Stop'),
              ),
            ]),
            if (running && _token.isNotEmpty) ...[
              const SizedBox(height: 12),
              _copyRow('bearer token', _token),
              const SizedBox(height: 8),
              const Text('Example request:'),
              SelectableText(_curlExample(),
                  style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
              TextButton.icon(
                onPressed: () => _copy('curl example', _curlExample()),
                icon: const Icon(Icons.copy, size: 16),
                label: const Text('Copy curl'),
              ),
            ],
          ]),
          _section('MCP server (bytecode-mcp)', [
            const Text('Point an MCP client at this command (stdio JSON-RPC):'),
            const SizedBox(height: 6),
            SelectableText(_mcpCommand(),
                style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
            TextButton.icon(
              onPressed: () => _copy('MCP command', _mcpCommand()),
              icon: const Icon(Icons.copy, size: 16),
              label: const Text('Copy command'),
            ),
          ]),
        ],
      ),
    );
  }

  Widget _section(String title, List<Widget> children) {
    return Card(
      margin: const EdgeInsets.only(bottom: 16),
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(title,
                style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 16)),
            const SizedBox(height: 12),
            ...children,
          ],
        ),
      ),
    );
  }

  Widget _copyRow(String label, String value) {
    return Row(children: [
      Expanded(
        child: SelectableText('$label: $value',
            style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
      ),
      IconButton(
        icon: const Icon(Icons.copy, size: 16),
        onPressed: () => _copy(label, value),
      ),
    ]);
  }
}
