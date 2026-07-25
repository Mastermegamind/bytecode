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
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xff0f766e),
          brightness: Brightness.light,
        ),
        scaffoldBackgroundColor: const Color(0xfff6f4ef),
        useMaterial3: true,
        fontFamily: 'Roboto',
        appBarTheme: const AppBarTheme(
          backgroundColor: Colors.transparent,
          foregroundColor: Color(0xff171717),
          elevation: 0,
          centerTitle: false,
        ),
        filledButtonTheme: FilledButtonThemeData(
          style: FilledButton.styleFrom(
            minimumSize: const Size(0, 44),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(8),
            ),
          ),
        ),
        outlinedButtonTheme: OutlinedButtonThemeData(
          style: OutlinedButton.styleFrom(
            minimumSize: const Size(0, 44),
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(8),
            ),
          ),
        ),
        iconButtonTheme: IconButtonThemeData(
          style: IconButton.styleFrom(
            shape: RoundedRectangleBorder(
              borderRadius: BorderRadius.circular(8),
            ),
          ),
        ),
        inputDecorationTheme: InputDecorationTheme(
          filled: true,
          fillColor: Colors.white,
          border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(8),
            borderSide: const BorderSide(color: Color(0xffd8d3c7)),
          ),
          enabledBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(8),
            borderSide: const BorderSide(color: Color(0xffd8d3c7)),
          ),
          focusedBorder: OutlineInputBorder(
            borderRadius: BorderRadius.circular(8),
            borderSide: const BorderSide(color: Color(0xff0f766e), width: 1.5),
          ),
          contentPadding: const EdgeInsets.symmetric(
            horizontal: 14,
            vertical: 13,
          ),
        ),
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
    final totalBytes = _manifestRows.fold<int>(
      0,
      (total, entry) => total + entry.size,
    );

    return Scaffold(
      body: SafeArea(
        child: LayoutBuilder(
          builder: (context, constraints) {
            final compact = constraints.maxWidth < 960;
            final controls = _Controls(
              rootController: _rootController,
              outputController: _outputController,
              keyController: _keyController,
              excludeController: _excludeController,
              sources: _sources,
              busy: _busy,
              status: _status,
              onPickRoot: () => _pickFolder(_rootController),
              onPickOutput: () => _pickFolder(_outputController),
              onAddFiles: _addFiles,
              onAddFolder: _addFolder,
              onRemoveSource: _removeSource,
              onClearSources: _clearSources,
              onGenerateKey: _generateKey,
              onDump: _dump,
              onVerify: _verify,
            );
            final results = _ResultsPanel(
              rows: _manifestRows,
              logText: _log.toString(),
            );

            return Padding(
              padding: EdgeInsets.all(compact ? 14 : 20),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  _Header(
                    busy: _busy,
                    sourceCount: _sources.length,
                    manifestCount: _manifestRows.length,
                    totalBytes: totalBytes,
                  ),
                  const SizedBox(height: 16),
                  Expanded(
                    child: compact
                        ? ListView(
                            children: [
                              controls,
                              const SizedBox(height: 14),
                              SizedBox(height: 620, child: results),
                            ],
                          )
                        : Row(
                            crossAxisAlignment: CrossAxisAlignment.stretch,
                            children: [
                              SizedBox(width: 420, child: controls),
                              const SizedBox(width: 16),
                              Expanded(child: results),
                            ],
                          ),
                  ),
                ],
              ),
            );
          },
        ),
      ),
    );
  }
}

class _Header extends StatelessWidget {
  const _Header({
    required this.busy,
    required this.sourceCount,
    required this.manifestCount,
    required this.totalBytes,
  });

  final bool busy;
  final int sourceCount;
  final int manifestCount;
  final int totalBytes;

  @override
  Widget build(BuildContext context) {
    final textTheme = Theme.of(context).textTheme;

    return Wrap(
      spacing: 14,
      runSpacing: 12,
      crossAxisAlignment: WrapCrossAlignment.center,
      alignment: WrapAlignment.spaceBetween,
      children: [
        Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Container(
              width: 46,
              height: 46,
              decoration: BoxDecoration(
                color: const Color(0xff1f2937),
                borderRadius: BorderRadius.circular(8),
              ),
              child: const Icon(Icons.memory, color: Color(0xfffff7ed)),
            ),
            const SizedBox(width: 12),
            Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  'Bytecode Encoder',
                  style: textTheme.headlineSmall?.copyWith(
                    fontWeight: FontWeight.w800,
                    color: const Color(0xff171717),
                    letterSpacing: 0,
                  ),
                ),
                Text(
                  busy ? 'Process running' : 'Ready for packaging',
                  style: textTheme.bodyMedium?.copyWith(
                    color: const Color(0xff6b6256),
                  ),
                ),
              ],
            ),
          ],
        ),
        Wrap(
          spacing: 10,
          runSpacing: 10,
          children: [
            _MetricPill(
              icon: Icons.inventory_2_outlined,
              label: 'Sources',
              value: sourceCount.toString(),
            ),
            _MetricPill(
              icon: Icons.table_rows_outlined,
              label: 'Manifest',
              value: manifestCount.toString(),
            ),
            _MetricPill(
              icon: Icons.data_usage,
              label: 'Bytes',
              value: _formatBytes(totalBytes),
            ),
          ],
        ),
      ],
    );
  }
}

class _MetricPill extends StatelessWidget {
  const _MetricPill({
    required this.icon,
    required this.label,
    required this.value,
  });

  final IconData icon;
  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 46,
      padding: const EdgeInsets.symmetric(horizontal: 12),
      decoration: BoxDecoration(
        color: Colors.white,
        border: Border.all(color: const Color(0xffddd6c8)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(icon, size: 18, color: const Color(0xff0f766e)),
          const SizedBox(width: 9),
          Column(
            mainAxisAlignment: MainAxisAlignment.center,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(
                value,
                style: Theme.of(context).textTheme.labelLarge?.copyWith(
                  fontWeight: FontWeight.w800,
                  color: const Color(0xff171717),
                ),
              ),
              Text(
                label,
                style: Theme.of(context).textTheme.labelSmall?.copyWith(
                  color: const Color(0xff7a7165),
                ),
              ),
            ],
          ),
        ],
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
    required this.status,
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
  final String? status;
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
    return _Panel(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _PanelTitle(
            icon: Icons.tune,
            title: 'Build Setup',
            trailing: busy
                ? const SizedBox.square(
                    dimension: 18,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  )
                : const Icon(Icons.check_circle, color: Color(0xff0f766e)),
          ),
          const SizedBox(height: 18),
          _PathField(
            label: 'Bytecode root',
            controller: rootController,
            icon: Icons.account_tree_outlined,
            onPick: busy ? null : onPickRoot,
          ),
          const SizedBox(height: 12),
          _PathField(
            label: 'Output folder',
            controller: outputController,
            icon: Icons.folder_copy_outlined,
            onPick: busy ? null : onPickOutput,
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              Expanded(
                child: FilledButton.icon(
                  onPressed: busy ? null : onAddFiles,
                  icon: const Icon(Icons.note_add),
                  label: const Text('Add Files'),
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: FilledButton.tonalIcon(
                  onPressed: busy ? null : onAddFolder,
                  icon: const Icon(Icons.create_new_folder),
                  label: const Text('Add Folder'),
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          _SourceList(
            sources: sources,
            busy: busy,
            onRemove: onRemoveSource,
            onClear: onClearSources,
          ),
          const SizedBox(height: 16),
          TextField(
            controller: keyController,
            obscureText: true,
            decoration: InputDecoration(
              labelText: 'BYTECODE_KEY',
              prefixIcon: const Icon(Icons.key),
              suffixIcon: IconButton(
                tooltip: 'Generate key',
                onPressed: busy ? null : onGenerateKey,
                icon: const Icon(Icons.auto_awesome),
              ),
            ),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: excludeController,
            minLines: 1,
            maxLines: 3,
            decoration: const InputDecoration(
              labelText: 'Exclude globs',
              prefixIcon: Icon(Icons.filter_alt_outlined),
            ),
          ),
          const SizedBox(height: 16),
          Row(
            children: [
              Expanded(
                flex: 3,
                child: FilledButton.icon(
                  onPressed: busy ? null : onDump,
                  icon: busy
                      ? const SizedBox.square(
                          dimension: 18,
                          child: CircularProgressIndicator(
                            strokeWidth: 2,
                            color: Colors.white,
                          ),
                        )
                      : const Icon(Icons.play_arrow),
                  label: const Text('Dump'),
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                flex: 2,
                child: OutlinedButton.icon(
                  onPressed: busy ? null : onVerify,
                  icon: const Icon(Icons.verified),
                  label: const Text('Verify'),
                ),
              ),
            ],
          ),
          const SizedBox(height: 12),
          _StatusBanner(status: status, busy: busy),
        ],
      ),
    );
  }
}

class _Panel extends StatelessWidget {
  const _Panel({required this.child});

  final Widget child;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: Colors.white,
        border: Border.all(color: const Color(0xffddd6c8)),
        borderRadius: BorderRadius.circular(8),
        boxShadow: const [
          BoxShadow(
            blurRadius: 28,
            offset: Offset(0, 12),
            color: Color(0x14000000),
          ),
        ],
      ),
      child: child,
    );
  }
}

class _PanelTitle extends StatelessWidget {
  const _PanelTitle({required this.icon, required this.title, this.trailing});

  final IconData icon;
  final String title;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Container(
          width: 34,
          height: 34,
          decoration: BoxDecoration(
            color: const Color(0xfffff7ed),
            borderRadius: BorderRadius.circular(8),
          ),
          child: Icon(icon, size: 19, color: const Color(0xffb45309)),
        ),
        const SizedBox(width: 10),
        Expanded(
          child: Text(
            title,
            style: Theme.of(context).textTheme.titleMedium?.copyWith(
              color: const Color(0xff171717),
              fontWeight: FontWeight.w800,
            ),
          ),
        ),
        ?trailing,
      ],
    );
  }
}

class _StatusBanner extends StatelessWidget {
  const _StatusBanner({required this.status, required this.busy});

  final String? status;
  final bool busy;

  @override
  Widget build(BuildContext context) {
    final message = status ?? (busy ? 'Working' : 'Idle');
    final isError =
        message.toLowerCase().contains('error') ||
        message.toLowerCase().contains('failed') ||
        message.toLowerCase().contains('required') ||
        message.toLowerCase().contains('must be') ||
        message.toLowerCase().contains('exited');
    final color = isError
        ? const Color(0xffb91c1c)
        : busy
        ? const Color(0xff92400e)
        : const Color(0xff0f766e);
    final background = isError
        ? const Color(0xffffeeee)
        : busy
        ? const Color(0xfffff7ed)
        : const Color(0xffecfdf5);

    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: background,
        border: Border.all(color: color.withValues(alpha: 0.25)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        children: [
          Icon(
            isError
                ? Icons.error_outline
                : busy
                ? Icons.sync
                : Icons.radio_button_checked,
            color: color,
            size: 20,
          ),
          const SizedBox(width: 10),
          Expanded(
            child: Text(
              message,
              maxLines: 3,
              overflow: TextOverflow.ellipsis,
              style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                color: const Color(0xff24211d),
                fontWeight: FontWeight.w600,
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _PathField extends StatelessWidget {
  const _PathField({
    required this.label,
    required this.controller,
    required this.icon,
    required this.onPick,
  });

  final String label;
  final TextEditingController controller;
  final IconData icon;
  final VoidCallback? onPick;

  @override
  Widget build(BuildContext context) {
    return TextField(
      controller: controller,
      decoration: InputDecoration(
        labelText: label,
        prefixIcon: Icon(icon),
        suffixIcon: IconButton(
          tooltip: 'Choose $label',
          onPressed: onPick,
          icon: const Icon(Icons.folder_open),
        ),
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
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xfffaf9f6),
        border: Border.all(color: const Color(0xffddd6c8)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: SizedBox(
        height: 176,
        child: sources.isEmpty
            ? const _EmptyState(
                icon: Icons.upload_file,
                title: 'No files or folders selected',
              )
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
                            color: const Color(0xff0f766e),
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
                    child: Padding(
                      padding: const EdgeInsets.only(right: 6),
                      child: TextButton.icon(
                        onPressed: busy ? null : onClear,
                        icon: const Icon(Icons.clear_all),
                        label: const Text('Clear'),
                      ),
                    ),
                  ),
                ],
              ),
      ),
    );
  }
}

class _ResultsPanel extends StatelessWidget {
  const _ResultsPanel({required this.rows, required this.logText});

  final List<ManifestEntry> rows;
  final String logText;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        final stacked = constraints.maxWidth < 760;

        return _Panel(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.stretch,
            children: [
              const _PanelTitle(
                icon: Icons.analytics_outlined,
                title: 'Artifacts',
              ),
              const SizedBox(height: 14),
              Expanded(
                child: stacked
                    ? Column(
                        children: [
                          Expanded(child: _ManifestTable(rows: rows)),
                          const SizedBox(height: 12),
                          Expanded(child: _LogPane(text: logText)),
                        ],
                      )
                    : Row(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          Expanded(flex: 7, child: _ManifestTable(rows: rows)),
                          const SizedBox(width: 12),
                          Expanded(flex: 5, child: _LogPane(text: logText)),
                        ],
                      ),
              ),
            ],
          ),
        );
      },
    );
  }
}

class _ManifestTable extends StatelessWidget {
  const _ManifestTable({required this.rows});

  final List<ManifestEntry> rows;

  @override
  Widget build(BuildContext context) {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xfffaf9f6),
        border: Border.all(color: const Color(0xffddd6c8)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: rows.isEmpty
          ? const _EmptyState(
              icon: Icons.table_chart_outlined,
              title: 'No manifest loaded',
            )
          : Scrollbar(
              child: SingleChildScrollView(
                scrollDirection: Axis.horizontal,
                child: SingleChildScrollView(
                  child: DataTable(
                    headingRowColor: WidgetStateProperty.all(
                      const Color(0xfffff7ed),
                    ),
                    headingTextStyle: Theme.of(context).textTheme.labelLarge
                        ?.copyWith(
                          color: const Color(0xff3f3529),
                          fontWeight: FontWeight.w800,
                        ),
                    dataTextStyle: Theme.of(context).textTheme.bodySmall,
                    columnSpacing: 26,
                    horizontalMargin: 16,
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
                            DataCell(Text(_formatBytes(row.size))),
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
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xff181512),
        border: Border.all(color: const Color(0xff2f2923)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: SingleChildScrollView(
          reverse: true,
          child: SelectableText(
            text.isEmpty ? 'Command output' : text,
            style: const TextStyle(
              color: Color(0xfff5efe7),
              fontFamily: 'monospace',
              fontSize: 12,
              height: 1.45,
            ),
          ),
        ),
      ),
    );
  }
}

class _EmptyState extends StatelessWidget {
  const _EmptyState({required this.icon, required this.title});

  final IconData icon;
  final String title;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Icon(icon, size: 34, color: const Color(0xff9a8f80)),
            const SizedBox(height: 10),
            Text(
              title,
              textAlign: TextAlign.center,
              style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                color: const Color(0xff6b6256),
                fontWeight: FontWeight.w600,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

String _formatBytes(int bytes) {
  if (bytes < 1024) {
    return '$bytes B';
  }

  const units = ['KB', 'MB', 'GB'];
  var size = bytes / 1024;
  var unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit++;
  }

  return '${size.toStringAsFixed(size >= 10 ? 0 : 1)} ${units[unit]}';
}
