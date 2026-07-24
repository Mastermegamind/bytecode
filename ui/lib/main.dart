import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:file_selector/file_selector.dart';
import 'package:flutter/material.dart';

void main() {
  runApp(const BytecodeApp());
}

class BytecodeApp extends StatelessWidget {
  const BytecodeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Bytecode Encoder',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xff2563eb),
          brightness: Brightness.light,
        ),
        useMaterial3: true,
      ),
      home: const EncoderPage(),
    );
  }
}

class ManifestEntry {
  const ManifestEntry({
    required this.source,
    required this.output,
    required this.size,
    required this.sha256,
  });

  final String source;
  final String output;
  final int size;
  final String sha256;

  factory ManifestEntry.fromJson(Map<String, dynamic> json) {
    return ManifestEntry(
      source: json['source'] as String? ?? '',
      output: json['output'] as String? ?? '',
      size: json['size'] as int? ?? 0,
      sha256: json['sha256'] as String? ?? '',
    );
  }
}

class EncoderPage extends StatefulWidget {
  const EncoderPage({super.key});

  @override
  State<EncoderPage> createState() => _EncoderPageState();
}

class _EncoderPageState extends State<EncoderPage> {
  late final TextEditingController _rootController;
  final _outputController = TextEditingController();
  final _keyController = TextEditingController();
  final _excludeController = TextEditingController(
    text: '.history/*, vendor/*, storage/*, node_modules/*',
  );

  final _log = StringBuffer();
  final _manifestRows = <ManifestEntry>[];
  final _sources = <String>[];

  bool _busy = false;
  String? _status;

  @override
  void initState() {
    super.initState();
    _rootController = TextEditingController(text: _defaultRepoRoot());
  }

  @override
  void dispose() {
    _rootController.dispose();
    _outputController.dispose();
    _keyController.dispose();
    _excludeController.dispose();
    super.dispose();
  }

  String _defaultRepoRoot() {
    final current = Directory.current;
    if (current.path.endsWith('${Platform.pathSeparator}ui')) {
      return current.parent.path;
    }
    return current.path;
  }

  String _join(String a, String b) {
    final sep = Platform.pathSeparator;
    return a.endsWith(sep) ? '$a$b' : '$a$sep$b';
  }

  String get _root => _rootController.text.trim();
  String get _manifestPath =>
      _join(_outputController.text.trim(), 'bytecode.manifest.json');

  Future<void> _pickFolder(TextEditingController controller) async {
    final path = await getDirectoryPath();
    if (path != null) {
      setState(() => controller.text = path);
    }
  }

  Future<void> _addFiles() async {
    final files = await openFiles();
    if (files.isEmpty) {
      return;
    }
    setState(() {
      for (final file in files) {
        if (!_sources.contains(file.path)) {
          _sources.add(file.path);
        }
      }
    });
  }

  Future<void> _addFolder() async {
    final path = await getDirectoryPath();
    if (path == null) {
      return;
    }
    setState(() {
      if (!_sources.contains(path)) {
        _sources.add(path);
      }
    });
  }

  void _removeSource(String source) {
    setState(() => _sources.remove(source));
  }

  void _clearSources() {
    setState(() => _sources.clear());
  }

  Future<void> _generateKey() async {
    await _runLocked(() async {
      final result = await Process.run('php', [
        _join(_root, 'php/bin/bytecode-keygen'),
      ], workingDirectory: _root);
      if (result.exitCode != 0) {
        _append(result.stderr.toString());
        throw StateError('key generation failed');
      }
      _keyController.text = result.stdout.toString().trim();
      _status = 'Key generated';
    });
  }

  List<String> _excludeArgs() {
    final values = _excludeController.text
        .split(',')
        .map((value) => value.trim())
        .where((value) => value.isNotEmpty);
    return [
      for (final value in values) ...['--exclude', value],
    ];
  }

  Future<void> _dump() async {
    final output = _outputController.text.trim();
    final key = _keyController.text.trim();
    if (_sources.isEmpty || output.isEmpty || key.isEmpty) {
      setState(
        () => _status = 'At least one source, output, and key are required',
      );
      return;
    }
    if (!RegExp(r'\A[0-9a-fA-F]{64}\z').hasMatch(key)) {
      setState(() => _status = 'Key must be 64 hex characters');
      return;
    }

    await _runLocked(() async {
      _log.clear();
      _manifestRows.clear();
      _status = 'Dumping';
      setState(() {});

      final process = await Process.start(
        'php',
        [
          _join(_root, 'php/bin/bytecode-dump'),
          ..._excludeArgs(),
          ..._sources,
          output,
        ],
        workingDirectory: _root,
        environment: {'BYTECODE_KEY': key},
      );

      final stdoutSub = process.stdout.transform(utf8.decoder).listen(_append);
      final stderrSub = process.stderr.transform(utf8.decoder).listen(_append);
      final exit = await process.exitCode;
      await stdoutSub.cancel();
      await stderrSub.cancel();

      if (exit != 0) {
        throw StateError('bytecode-dump exited with $exit');
      }

      await _loadManifest();
      _status = 'Dump complete';
    });
  }

  Future<void> _verify() async {
    await _runLocked(() async {
      _status = 'Verifying';
      setState(() {});
      final result = await Process.run('php', [
        _join(_root, 'php/bin/bytecode-verify'),
        _manifestPath,
      ], workingDirectory: _root);
      _append(result.stdout.toString());
      _append(result.stderr.toString());
      if (result.exitCode != 0) {
        throw StateError('bytecode-verify exited with ${result.exitCode}');
      }
      _status = result.stdout.toString().trim();
    });
  }

  Future<void> _loadManifest() async {
    final file = File(_manifestPath);
    if (!await file.exists()) {
      throw StateError('manifest not found: $_manifestPath');
    }
    final decoded =
        jsonDecode(await file.readAsString()) as Map<String, dynamic>;
    final files = decoded['files'] as List<dynamic>? ?? [];
    _manifestRows
      ..clear()
      ..addAll(
        files.map(
          (entry) => ManifestEntry.fromJson(entry as Map<String, dynamic>),
        ),
      );
  }

  void _append(String text) {
    if (text.isEmpty) {
      return;
    }
    setState(() {
      _log.write(text);
    });
  }

  Future<void> _runLocked(Future<void> Function() action) async {
    if (_busy) {
      return;
    }
    setState(() {
      _busy = true;
      _status = null;
    });
    try {
      await action();
    } catch (error) {
      _status = error.toString();
    } finally {
      if (mounted) {
        setState(() => _busy = false);
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Bytecode Encoder')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          children: [
            _Controls(
              rootController: _rootController,
              outputController: _outputController,
              keyController: _keyController,
              excludeController: _excludeController,
              sources: _sources,
              busy: _busy,
              onPickRoot: () => _pickFolder(_rootController),
              onPickOutput: () => _pickFolder(_outputController),
              onAddFiles: _addFiles,
              onAddFolder: _addFolder,
              onRemoveSource: _removeSource,
              onClearSources: _clearSources,
              onGenerateKey: _generateKey,
              onDump: _dump,
              onVerify: _verify,
            ),
            const SizedBox(height: 12),
            if (_status != null)
              Align(
                alignment: Alignment.centerLeft,
                child: Text(
                  _status!,
                  style: Theme.of(context).textTheme.bodyMedium,
                ),
              ),
            const SizedBox(height: 12),
            Expanded(
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Expanded(flex: 5, child: _ManifestTable(rows: _manifestRows)),
                  const SizedBox(width: 12),
                  Expanded(flex: 4, child: _LogPane(text: _log.toString())),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _Controls extends StatelessWidget {
  const _Controls({
    required this.rootController,
    required this.outputController,
    required this.keyController,
    required this.excludeController,
    required this.sources,
    required this.busy,
    required this.onPickRoot,
    required this.onPickOutput,
    required this.onAddFiles,
    required this.onAddFolder,
    required this.onRemoveSource,
    required this.onClearSources,
    required this.onGenerateKey,
    required this.onDump,
    required this.onVerify,
  });

  final TextEditingController rootController;
  final TextEditingController outputController;
  final TextEditingController keyController;
  final TextEditingController excludeController;
  final List<String> sources;
  final bool busy;
  final VoidCallback onPickRoot;
  final VoidCallback onPickOutput;
  final VoidCallback onAddFiles;
  final VoidCallback onAddFolder;
  final ValueChanged<String> onRemoveSource;
  final VoidCallback onClearSources;
  final VoidCallback onGenerateKey;
  final VoidCallback onDump;
  final VoidCallback onVerify;

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Row(
          children: [
            Expanded(
              child: _PathField(
                label: 'Bytecode root',
                controller: rootController,
              ),
            ),
            const SizedBox(width: 8),
            IconButton(
              tooltip: 'Choose bytecode root',
              onPressed: busy ? null : onPickRoot,
              icon: const Icon(Icons.folder_open),
            ),
          ],
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            FilledButton.icon(
              onPressed: busy ? null : onAddFiles,
              icon: const Icon(Icons.note_add),
              label: const Text('Add Files'),
            ),
            const SizedBox(width: 8),
            FilledButton.tonalIcon(
              onPressed: busy ? null : onAddFolder,
              icon: const Icon(Icons.create_new_folder),
              label: const Text('Add Folder'),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _PathField(
                label: 'Output folder',
                controller: outputController,
              ),
            ),
            const SizedBox(width: 8),
            IconButton(
              tooltip: 'Choose output folder',
              onPressed: busy ? null : onPickOutput,
              icon: const Icon(Icons.create_new_folder),
            ),
          ],
        ),
        const SizedBox(height: 8),
        _SourceList(
          sources: sources,
          busy: busy,
          onRemove: onRemoveSource,
          onClear: onClearSources,
        ),
        const SizedBox(height: 8),
        Row(
          children: [
            Expanded(
              flex: 3,
              child: TextField(
                controller: keyController,
                obscureText: true,
                decoration: const InputDecoration(
                  labelText: 'BYTECODE_KEY',
                  border: OutlineInputBorder(),
                ),
              ),
            ),
            const SizedBox(width: 8),
            IconButton(
              tooltip: 'Generate key',
              onPressed: busy ? null : onGenerateKey,
              icon: const Icon(Icons.key),
            ),
            const SizedBox(width: 12),
            Expanded(
              flex: 4,
              child: TextField(
                controller: excludeController,
                decoration: const InputDecoration(
                  labelText: 'Exclude globs',
                  border: OutlineInputBorder(),
                ),
              ),
            ),
            const SizedBox(width: 12),
            FilledButton.icon(
              onPressed: busy ? null : onDump,
              icon: busy
                  ? const SizedBox.square(
                      dimension: 18,
                      child: CircularProgressIndicator(strokeWidth: 2),
                    )
                  : const Icon(Icons.play_arrow),
              label: const Text('Dump'),
            ),
            const SizedBox(width: 8),
            OutlinedButton.icon(
              onPressed: busy ? null : onVerify,
              icon: const Icon(Icons.verified),
              label: const Text('Verify'),
            ),
          ],
        ),
      ],
    );
  }
}

class _PathField extends StatelessWidget {
  const _PathField({required this.label, required this.controller});

  final String label;
  final TextEditingController controller;

  @override
  Widget build(BuildContext context) {
    return TextField(
      controller: controller,
      decoration: InputDecoration(
        labelText: label,
        border: const OutlineInputBorder(),
      ),
    );
  }
}

class _SourceList extends StatelessWidget {
  const _SourceList({
    required this.sources,
    required this.busy,
    required this.onRemove,
    required this.onClear,
  });

  final List<String> sources;
  final bool busy;
  final ValueChanged<String> onRemove;
  final VoidCallback onClear;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        border: Border.all(color: Theme.of(context).colorScheme.outlineVariant),
        borderRadius: BorderRadius.circular(8),
      ),
      child: SizedBox(
        height: 108,
        child: sources.isEmpty
            ? const Center(child: Text('No files or folders selected'))
            : Column(
                children: [
                  Expanded(
                    child: ListView.separated(
                      itemCount: sources.length,
                      separatorBuilder: (_, _) => const Divider(height: 1),
                      itemBuilder: (context, index) {
                        final source = sources[index];
                        return ListTile(
                          dense: true,
                          leading: Icon(
                            FileSystemEntity.isDirectorySync(source)
                                ? Icons.folder
                                : Icons.description,
                          ),
                          title: Text(
                            source,
                            maxLines: 1,
                            overflow: TextOverflow.ellipsis,
                          ),
                          trailing: IconButton(
                            tooltip: 'Remove source',
                            onPressed: busy ? null : () => onRemove(source),
                            icon: const Icon(Icons.close),
                          ),
                        );
                      },
                    ),
                  ),
                  Align(
                    alignment: Alignment.centerRight,
                    child: TextButton.icon(
                      onPressed: busy ? null : onClear,
                      icon: const Icon(Icons.clear_all),
                      label: const Text('Clear'),
                    ),
                  ),
                ],
              ),
      ),
    );
  }
}

class _ManifestTable extends StatelessWidget {
  const _ManifestTable({required this.rows});

  final List<ManifestEntry> rows;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        border: Border.all(color: Theme.of(context).colorScheme.outlineVariant),
        borderRadius: BorderRadius.circular(8),
      ),
      child: rows.isEmpty
          ? const Center(child: Text('No manifest loaded'))
          : Scrollbar(
              child: SingleChildScrollView(
                scrollDirection: Axis.horizontal,
                child: SingleChildScrollView(
                  child: DataTable(
                    columns: const [
                      DataColumn(label: Text('Source')),
                      DataColumn(label: Text('Output')),
                      DataColumn(label: Text('Size')),
                      DataColumn(label: Text('SHA-256')),
                    ],
                    rows: [
                      for (final row in rows)
                        DataRow(
                          cells: [
                            DataCell(SelectableText(row.source)),
                            DataCell(SelectableText(row.output)),
                            DataCell(Text(row.size.toString())),
                            DataCell(SelectableText(row.sha256)),
                          ],
                        ),
                    ],
                  ),
                ),
              ),
            ),
    );
  }
}

class _LogPane extends StatelessWidget {
  const _LogPane({required this.text});

  final String text;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: const Color(0xff111827),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: SingleChildScrollView(
          reverse: true,
          child: SelectableText(
            text.isEmpty ? 'Command output' : text,
            style: const TextStyle(
              color: Color(0xffe5e7eb),
              fontFamily: 'monospace',
              fontSize: 12,
            ),
          ),
        ),
      ),
    );
  }
}
