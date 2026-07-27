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
    required this.backend,
    required this.contentType,
    required this.size,
    required this.sha256,
  });

  final String source;
  final String output;
  final String backend;
  final String contentType;
  final int size;
  final String sha256;

  factory ManifestEntry.fromJson(Map<String, dynamic> json) {
    return ManifestEntry(
      source: json['source'] as String? ?? '',
      output: json['output'] as String? ?? '',
      backend: json['backend'] as String? ?? '',
      contentType: json['content_type'] as String? ?? '',
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
  final _phpVersionController = TextEditingController(text: '8.4');
  final _configController = TextEditingController();
  final _licensePubkeyController = TextEditingController();
  final _machineIdController = TextEditingController();
  final _licenseKeyDirController = TextEditingController();
  final _vendorSignKeyController = TextEditingController();
  final _vendorKeyDirController = TextEditingController();
  final _inspectPathController = TextEditingController();
  final _packageVersionController = TextEditingController(text: '1.0.0');
  final _profileController = TextEditingController(text: 'php-assets');
  final _remoteTargetController = TextEditingController(
    text: 'trigger@192.168.8.42:/var/www/exam.test',
  );
  final _remoteStageController = TextEditingController(
    text: '/var/www/exam.test.stage',
  );
  final _remoteRoutesController = TextEditingController(
    text: '/,/login,/backend/login',
  );
  final _remotePortController = TextEditingController(text: '8095');
  final _manifestFilterController = TextEditingController();
  final _excludeController = TextEditingController(
    text: '.history/*, vendor/*, storage/*, node_modules/*',
  );

  final _log = StringBuffer();
  final _manifestRows = <ManifestEntry>[];
  final _sources = <String>[];

  bool _busy = false;
  bool _rawContainers = false;
  bool _dryRun = false;
  bool _includeAssets = false;
  bool _obfuscate = false;
  bool _scanBeforeDump = true;
  bool _failOnScanWarning = false;
  bool _deployStartRunner = false;
  String? _status;

  @override
  void initState() {
    super.initState();
    _rootController = TextEditingController(text: _defaultRepoRoot());
    _keyController.text = _vendorSecretPath;
    _rootController.addListener(() {
      final path = _vendorSecretPath;
      if (_keyController.text != path) {
        _keyController.text = path;
      }
    });
  }

  @override
  void dispose() {
    _rootController.dispose();
    _outputController.dispose();
    _keyController.dispose();
    _phpVersionController.dispose();
    _configController.dispose();
    _licensePubkeyController.dispose();
    _machineIdController.dispose();
    _licenseKeyDirController.dispose();
    _vendorSignKeyController.dispose();
    _vendorKeyDirController.dispose();
    _inspectPathController.dispose();
    _packageVersionController.dispose();
    _profileController.dispose();
    _remoteTargetController.dispose();
    _remoteStageController.dispose();
    _remoteRoutesController.dispose();
    _remotePortController.dispose();
    _manifestFilterController.dispose();
    _excludeController.dispose();
    super.dispose();
  }

  String _defaultRepoRoot() {
    final envRoot = Platform.environment['BYTECODE_ROOT'];
    if (envRoot != null &&
        envRoot.isNotEmpty &&
        Directory(envRoot).existsSync()) {
      return envRoot;
    }

    final appDir = Platform.environment['APPDIR'];
    if (appDir != null && appDir.isNotEmpty) {
      final bundledRoot = _join(_join(appDir, 'usr'), 'lib/bytecode');
      if (Directory(bundledRoot).existsSync()) {
        return bundledRoot;
      }
    }

    final executable = File(Platform.resolvedExecutable);
    final executableDir = executable.parent.path;

    if (Platform.isMacOS) {
      final contentsDir = executable.parent.parent.path;
      final bundledRoot = _join(_join(contentsDir, 'Resources'), 'bytecode');
      if (Directory(bundledRoot).existsSync()) {
        return bundledRoot;
      }
    }

    if (Platform.isWindows) {
      final bundledRoot = _join(executableDir, 'bytecode');
      if (Directory(bundledRoot).existsSync()) {
        return bundledRoot;
      }
    }

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

  String _phpBinary() {
    final envPhp = Platform.environment['BYTECODE_PHP'];
    if (envPhp != null && envPhp.isNotEmpty && File(envPhp).existsSync()) {
      return envPhp;
    }

    final appDir = Platform.environment['APPDIR'];
    if (appDir != null && appDir.isNotEmpty) {
      final bundledPhp = _join(_join(appDir, 'usr'), 'bin/php');
      if (File(bundledPhp).existsSync()) {
        return bundledPhp;
      }
    }

    final executable = File(Platform.resolvedExecutable);
    final executableDir = executable.parent.path;

    if (Platform.isMacOS) {
      final contentsDir = executable.parent.parent.path;
      final bundledPhp = _join(
        _join(_join(contentsDir, 'Resources'), 'php'),
        'bin/php',
      );
      if (File(bundledPhp).existsSync()) {
        return bundledPhp;
      }
    }

    if (Platform.isWindows) {
      final bundledPhp = _join(_join(executableDir, 'php'), 'php.exe');
      if (File(bundledPhp).existsSync()) {
        return bundledPhp;
      }
    }

    return 'php';
  }

  String get _root => _rootController.text.trim();
  String get _vendorSecretPath =>
      _join(_join(_root, 'build'), 'vendor-secret.key');
  String get _manifestPath =>
      _join(_outputController.text.trim(), 'bytecode.manifest.json');

  Future<void> _pickFolder(TextEditingController controller) async {
    final path = await getDirectoryPath();
    if (path != null) {
      setState(() => controller.text = path);
    }
  }

  Future<void> _pickRoot() async {
    final path = await getDirectoryPath();
    if (path != null) {
      setState(() {
        _rootController.text = path;
        _keyController.text = _vendorSecretPath;
      });
    }
  }

  Future<void> _pickFile(TextEditingController controller) async {
    final file = await openFile();
    if (file != null) {
      setState(() => controller.text = file.path);
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
      final result = await Process.run(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-keygen'),
        '--vendor-secret',
        '--force',
      ], workingDirectory: _root);
      if (result.exitCode != 0) {
        _append(result.stderr.toString());
        throw StateError('vendor secret regeneration failed');
      }
      _keyController.text = result.stdout.toString().trim();
      _status = 'Vendor secret regenerated';
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

  List<String> _configArgs() {
    final config = _configController.text.trim();
    return config.isEmpty ? [] : ['--config', config];
  }

  List<String> _profileArgs() {
    final profile = _profileController.text.trim();
    return profile.isEmpty ? [] : ['--profile', profile];
  }

  Future<int> _runStreaming(
    String executable,
    List<String> args, {
    Map<String, String>? environment,
  }) async {
    final process = await Process.start(
      executable,
      args,
      workingDirectory: _root,
      environment: environment,
    );

    final stdoutSub = process.stdout.transform(utf8.decoder).listen(_append);
    final stderrSub = process.stderr.transform(utf8.decoder).listen(_append);
    final exit = await process.exitCode;
    await stdoutSub.cancel();
    await stderrSub.cancel();
    return exit;
  }

  Future<void> _dump() async {
    final output = _outputController.text.trim();
    final licensePubkey = _licensePubkeyController.text.trim();
    final vendorSignKey = _vendorSignKeyController.text.trim();
    if (_sources.isEmpty || output.isEmpty) {
      setState(() => _status = 'At least one source and output are required');
      return;
    }

    await _runLocked(() async {
      _log.clear();
      _manifestRows.clear();
      _status = 'Dumping';
      setState(() {});

      final env = <String, String>{};
      if (!_rawContainers && licensePubkey.isNotEmpty) {
        env['BYTECODE_LICENSE_PUBKEY'] = licensePubkey;
      } else if (!_rawContainers) {
        env['BYTECODE_VENDOR_KEY_FILE'] = _vendorSecretPath;
      }
      if (!_rawContainers && vendorSignKey.isNotEmpty) {
        env['BYTECODE_VENDOR_SIGN_KEY'] = vendorSignKey;
      }

      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-dump'),
        if (_rawContainers) '--raw',
        if (_dryRun) '--dry-run',
        if (_includeAssets && !_rawContainers) '--include-assets',
        if (_obfuscate) '--obfuscate',
        if (_scanBeforeDump) '--scan',
        if (_failOnScanWarning) '--fail-on-scan-warning',
        if (_machineIdController.text.trim().isNotEmpty) ...[
          '--machine-id',
          _machineIdController.text.trim(),
        ],
        if (_vendorSignKeyController.text.trim().isNotEmpty) ...[
          '--vendor-sign-key',
          _vendorSignKeyController.text.trim(),
        ],
        ..._profileArgs(),
        ..._configArgs(),
        ..._excludeArgs(),
        ..._sources,
        output,
      ], environment: env);

      if (exit != 0) {
        throw StateError('bytecode-dump exited with $exit');
      }

      if (_dryRun) {
        _status = 'Dry run complete';
      } else {
        await _loadManifest();
        _status = 'Dump complete';
      }
    });
  }

  Future<void> _doctor() async {
    await _runLocked(() async {
      _log.clear();
      _status = 'Running doctor';
      setState(() {});
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-doctor'),
        '--php-version',
        _phpVersionController.text.trim().isEmpty
            ? '8.4'
            : _phpVersionController.text.trim(),
      ]);
      if (exit != 0) {
        throw StateError('bytecode-doctor exited with $exit');
      }
      _status = 'Doctor checks passed';
    });
  }

  Future<void> _rotateKey() async {
    final output = _outputController.text.trim();
    if (_sources.isEmpty || output.isEmpty) {
      setState(() => _status = 'At least one source and output are required');
      return;
    }
    await _runLocked(() async {
      _log.clear();
      _manifestRows.clear();
      _status = 'Rotating key and re-encoding';
      setState(() {});
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-key-rotate'),
        '--force',
        '--source',
        _sources.first,
        '--out',
        output,
        if (_includeAssets && !_rawContainers) '--include-assets',
        ..._profileArgs(),
        ..._excludeArgs(),
      ]);
      if (exit != 0) {
        throw StateError('bytecode-key-rotate exited with $exit');
      }
      await _loadManifest();
      _keyController.text = _vendorSecretPath;
      _status = 'Key rotated and output re-encoded';
    });
  }

  Future<void> _scanSources() async {
    if (_sources.isEmpty) {
      setState(() => _status = 'At least one source is required');
      return;
    }

    await _runLocked(() async {
      _log.clear();
      _status = 'Scanning sources';
      setState(() {});

      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-scan'),
        if (_failOnScanWarning) '--fail-on-warning',
        ..._configArgs(),
        ..._excludeArgs(),
        ..._sources,
      ]);

      if (exit != 0) {
        throw StateError('bytecode-scan exited with $exit');
      }
      _status = 'Scan complete';
    });
  }

  Future<void> _verify() async {
    await _verifyWith(decryptTest: false);
  }

  Future<void> _verifyWith({required bool decryptTest}) async {
    await _runLocked(() async {
      _status = 'Verifying';
      setState(() {});
      // When a vendor sign key is configured, hand bytecode-verify the sibling
      // public key so it checks the Ed25519 seal signature, not just digests.
      final env = <String, String>{};
      final vendorSignKey = _vendorSignKeyController.text.trim();
      if (vendorSignKey.isNotEmpty) {
        env['BYTECODE_VENDOR_PUBKEY'] = vendorSignKey.replaceAll(
          RegExp(r'vendor\.sign\.key\.pem$'),
          'vendor.sign.pub.pem',
        );
      }
      final result = await Process.run(
        _phpBinary(),
        [
          _join(_root, 'php/bin/bytecode-verify'),
          if (decryptTest) '--decrypt-test',
          _manifestPath,
        ],
        workingDirectory: _root,
        environment: env.isEmpty ? null : env,
      );
      _append(result.stdout.toString());
      _append(result.stderr.toString());
      if (result.exitCode != 0) {
        throw StateError('bytecode-verify exited with ${result.exitCode}');
      }
      _status = result.stdout.toString().trim();
    });
  }

  Future<void> _selfTest() async {
    await _runLocked(() async {
      _log.clear();
      _status = 'Running self-test';
      setState(() {});
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-selftest'),
      ]);
      if (exit != 0) {
        throw StateError('bytecode-selftest exited with $exit');
      }
      _status = 'Self-test passed';
    });
  }

  Future<void> _packageSign() async {
    final output = _outputController.text.trim();
    if (output.isEmpty) {
      setState(() => _status = 'Output folder is required');
      return;
    }
    await _runLocked(() async {
      _log.clear();
      _status = 'Signing package';
      setState(() {});
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-package-sign'),
        output,
      ]);
      if (exit != 0) {
        throw StateError('bytecode-package-sign exited with $exit');
      }
      _status = 'Package signed';
    });
  }

  Future<void> _saveProjectPreset() async {
    final file = File(_join(_join(_root, 'build'), 'gui-project.json'));
    await file.parent.create(recursive: true);
    final data = {
      'sources': _sources,
      'output': _outputController.text,
      'profile': _profileController.text,
      'excludes': _excludeController.text,
      'remote_target': _remoteTargetController.text,
      'remote_stage': _remoteStageController.text,
      'routes': _remoteRoutesController.text,
      'port': _remotePortController.text,
      'include_assets': _includeAssets,
      'scan': _scanBeforeDump,
    };
    await file.writeAsString(const JsonEncoder.withIndent('  ').convert(data));
    setState(() => _status = 'Project preset saved');
  }

  Future<void> _loadProjectPreset() async {
    final file = File(_join(_join(_root, 'build'), 'gui-project.json'));
    if (!await file.exists()) {
      setState(() => _status = 'No saved project preset found');
      return;
    }
    final data = jsonDecode(await file.readAsString()) as Map<String, dynamic>;
    setState(() {
      _sources
        ..clear()
        ..addAll((data['sources'] as List<dynamic>? ?? []).cast<String>());
      _outputController.text = data['output'] as String? ?? '';
      _profileController.text = data['profile'] as String? ?? 'php-assets';
      _excludeController.text = data['excludes'] as String? ?? '';
      _remoteTargetController.text = data['remote_target'] as String? ?? '';
      _remoteStageController.text = data['remote_stage'] as String? ?? '';
      _remoteRoutesController.text = data['routes'] as String? ?? '';
      _remotePortController.text = data['port'] as String? ?? '8095';
      _includeAssets = data['include_assets'] as bool? ?? _includeAssets;
      _scanBeforeDump = data['scan'] as bool? ?? _scanBeforeDump;
      _status = 'Project preset loaded';
    });
  }

  Future<void> _inspectContainer() async {
    final path = _inspectPathController.text.trim();
    if (path.isEmpty) {
      setState(() => _status = 'Choose an encoded container to inspect');
      return;
    }

    await _runLocked(() async {
      _log.clear();
      _status = 'Inspecting container';
      setState(() {});
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-info'),
        path,
      ]);
      if (exit != 0) {
        throw StateError('bytecode-info exited with $exit');
      }
      _status = 'Container inspected';
    });
  }

  Future<void> _generateLicenseKeys() async {
    final output = _licenseKeyDirController.text.trim();
    if (output.isEmpty) {
      setState(() => _status = 'Choose a license key output folder');
      return;
    }

    await _runLocked(() async {
      _log.clear();
      _status = 'Generating license keys';
      setState(() {});
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-license-keygen'),
        output,
      ]);
      if (exit != 0) {
        throw StateError('license key generation exited with $exit');
      }
      _licensePubkeyController.text = _join(output, 'license.pub.pem');
      _status = 'License keys generated';
    });
  }

  Future<void> _generateVendorKeys() async {
    final output = _vendorKeyDirController.text.trim();
    if (output.isEmpty) {
      setState(() => _status = 'Choose a vendor key output folder');
      return;
    }

    await _runLocked(() async {
      _log.clear();
      _status = 'Generating vendor signing keys';
      setState(() {});
      // Streams the compile-in hex (--with-opdump-vendor-pubkey) into the log.
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-vendor-keygen'),
        output,
      ]);
      if (exit != 0) {
        throw StateError('vendor key generation exited with $exit');
      }
      _vendorSignKeyController.text = _join(output, 'vendor.sign.key.pem');
      _status = 'Vendor signing keys generated';
    });
  }

  Future<void> _installLoader({required bool buildOnly}) async {
    final phpVersion = _phpVersionController.text.trim();
    if (!RegExp(r'\A\d+\.\d+\z').hasMatch(phpVersion)) {
      setState(() => _status = 'PHP version must look like 8.4');
      return;
    }

    await _runLocked(() async {
      _log.clear();
      _status = buildOnly ? 'Building loader' : 'Installing loader';
      setState(() {});

      final args = [
        _join(_root, 'php/bin/bytecode-install-loader'),
        '--php-version',
        phpVersion,
        if (buildOnly) '--build-only',
      ];
      final exit = await _runStreaming(_phpBinary(), args);

      if (exit != 0) {
        throw StateError(
          buildOnly
              ? 'loader build exited with $exit'
              : 'loader install exited with $exit',
        );
      }

      _status = buildOnly ? 'Loader build complete' : 'Loader installed';
    });
  }

  Future<void> _deploy({required bool cutover}) async {
    final target = _remoteTargetController.text.trim();
    final stage = _remoteStageController.text.trim();
    if (target.isEmpty || stage.isEmpty) {
      setState(() => _status = 'Remote target and stage are required');
      return;
    }
    await _runLocked(() async {
      _log.clear();
      _status = cutover ? 'Deploying and cutting over' : 'Deploying to stage';
      setState(() {});
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-deploy'),
        target,
        '--stage',
        stage,
        '--local-root',
        _sources.isNotEmpty ? _sources.first : Directory.current.path,
        '--bytecode-root',
        _root,
        if (_includeAssets && !_rawContainers) '--include-assets',
        if (_machineIdController.text.trim().isNotEmpty) ...[
          '--machine-id',
          _machineIdController.text.trim(),
        ],
        if (_vendorSignKeyController.text.trim().isNotEmpty) ...[
          '--vendor-sign-key',
          _vendorSignKeyController.text.trim(),
        ],
        ..._profileArgs(),
        ..._excludeArgs(),
        if (_remoteRoutesController.text.trim().isNotEmpty) ...[
          '--verify-routes',
          _remoteRoutesController.text.trim(),
        ],
        '--port',
        _remotePortController.text.trim().isEmpty
            ? '8095'
            : _remotePortController.text.trim(),
        if (_deployStartRunner) '--start-runner',
        if (cutover) '--cutover',
      ]);
      if (exit != 0) {
        throw StateError('bytecode-deploy exited with $exit');
      }
      _status = cutover ? 'Remote cutover complete' : 'Remote stage complete';
    });
  }

  Future<void> _deployRollback() async {
    final target = _remoteTargetController.text.trim();
    final stage = _remoteStageController.text.trim();
    if (target.isEmpty || stage.isEmpty) {
      setState(() => _status = 'Remote target and stage are required');
      return;
    }
    await _runLocked(() async {
      _log.clear();
      _status = 'Rolling back remote app';
      setState(() {});
      final exit = await _runStreaming(_phpBinary(), [
        _join(_root, 'php/bin/bytecode-deploy'),
        target,
        '--stage',
        stage,
        '--rollback',
      ]);
      if (exit != 0) {
        throw StateError('bytecode-deploy rollback exited with $exit');
      }
      _status = 'Remote rollback complete';
    });
  }

  Future<void> _buildDesktopPackage(String kind) async {
    final phpVersion = _phpVersionController.text.trim();
    final version = _packageVersionController.text.trim();
    await _runLocked(() async {
      _log.clear();
      _status = 'Building $kind package';
      setState(() {});

      late final String executable;
      late final List<String> args;
      final env = <String, String>{
        'PHP_VERSION': phpVersion.isEmpty ? '8.4' : phpVersion,
        'VERSION': version.isEmpty ? '1.0.0' : version,
      };

      switch (kind) {
        case 'AppImage':
          executable = 'bash';
          args = [_join(_root, 'scripts/build-linux-appimage.sh')];
        case 'Debian':
          executable = 'bash';
          args = [_join(_root, 'scripts/build-linux-deb.sh')];
        case 'macOS':
          executable = 'bash';
          args = [_join(_root, 'scripts/build-macos-app.sh')];
        case 'Windows ZIP':
          executable = Platform.isWindows ? 'powershell' : 'pwsh';
          args = [
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            _join(_root, 'scripts/build-windows-package.ps1'),
            '-Version',
            env['VERSION']!,
          ];
        case 'Windows MSI':
          executable = Platform.isWindows ? 'powershell' : 'pwsh';
          args = [
            '-ExecutionPolicy',
            'Bypass',
            '-File',
            _join(_root, 'scripts/build-windows-package.ps1'),
            '-BuildMsi',
            '-Version',
            env['VERSION']!,
          ];
        default:
          throw StateError('unknown package kind: $kind');
      }

      final exit = await _runStreaming(executable, args, environment: env);
      if (exit != 0) {
        throw StateError('$kind package build exited with $exit');
      }
      _status = '$kind package built';
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
    if (_inspectPathController.text.trim().isEmpty &&
        _manifestRows.isNotEmpty) {
      _inspectPathController.text = _join(
        _outputController.text.trim(),
        _manifestRows.first.output,
      );
    }
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
              phpVersionController: _phpVersionController,
              configController: _configController,
              licensePubkeyController: _licensePubkeyController,
              machineIdController: _machineIdController,
              licenseKeyDirController: _licenseKeyDirController,
              vendorSignKeyController: _vendorSignKeyController,
              vendorKeyDirController: _vendorKeyDirController,
              inspectPathController: _inspectPathController,
              packageVersionController: _packageVersionController,
              profileController: _profileController,
              remoteTargetController: _remoteTargetController,
              remoteStageController: _remoteStageController,
              remoteRoutesController: _remoteRoutesController,
              remotePortController: _remotePortController,
              manifestFilterController: _manifestFilterController,
              excludeController: _excludeController,
              sources: _sources,
              busy: _busy,
              status: _status,
              rawContainers: _rawContainers,
              dryRun: _dryRun,
              includeAssets: _includeAssets,
              obfuscate: _obfuscate,
              scanBeforeDump: _scanBeforeDump,
              failOnScanWarning: _failOnScanWarning,
              deployStartRunner: _deployStartRunner,
              onPickRoot: _pickRoot,
              onPickOutput: () => _pickFolder(_outputController),
              onPickConfig: () => _pickFile(_configController),
              onPickLicensePubkey: () => _pickFile(_licensePubkeyController),
              onPickLicenseKeyDir: () => _pickFolder(_licenseKeyDirController),
              onPickVendorSignKey: () => _pickFile(_vendorSignKeyController),
              onPickVendorKeyDir: () => _pickFolder(_vendorKeyDirController),
              onPickInspectPath: () => _pickFile(_inspectPathController),
              onAddFiles: _addFiles,
              onAddFolder: _addFolder,
              onRemoveSource: _removeSource,
              onClearSources: _clearSources,
              onGenerateKey: _generateKey,
              onGenerateLicenseKeys: _generateLicenseKeys,
              onGenerateVendorKeys: _generateVendorKeys,
              onDoctor: _doctor,
              onSelfTest: _selfTest,
              onRotateKey: _rotateKey,
              onScan: _scanSources,
              onDump: _dump,
              onVerify: _verify,
              onDecryptVerify: () => _verifyWith(decryptTest: true),
              onInspect: _inspectContainer,
              onPackageSign: _packageSign,
              onSaveProject: _saveProjectPreset,
              onLoadProject: _loadProjectPreset,
              onBuildLoader: () => _installLoader(buildOnly: true),
              onInstallLoader: () => _installLoader(buildOnly: false),
              onBuildAppImage: () => _buildDesktopPackage('AppImage'),
              onBuildDeb: () => _buildDesktopPackage('Debian'),
              onBuildMac: () => _buildDesktopPackage('macOS'),
              onBuildWindowsZip: () => _buildDesktopPackage('Windows ZIP'),
              onBuildWindowsMsi: () => _buildDesktopPackage('Windows MSI'),
              onDeployStage: () => _deploy(cutover: false),
              onDeployCutover: () => _deploy(cutover: true),
              onDeployRollback: _deployRollback,
              onRawContainersChanged: (value) =>
                  setState(() => _rawContainers = value),
              onDryRunChanged: (value) => setState(() => _dryRun = value),
              onIncludeAssetsChanged: (value) =>
                  setState(() => _includeAssets = value),
              onObfuscateChanged: (value) => setState(() => _obfuscate = value),
              onScanBeforeDumpChanged: (value) =>
                  setState(() => _scanBeforeDump = value),
              onFailOnScanWarningChanged: (value) =>
                  setState(() => _failOnScanWarning = value),
              onDeployStartRunnerChanged: (value) =>
                  setState(() => _deployStartRunner = value),
            );
            final results = _ResultsPanel(
              rows: _manifestRows,
              logText: _log.toString(),
              filterController: _manifestFilterController,
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
    required this.phpVersionController,
    required this.configController,
    required this.licensePubkeyController,
    required this.machineIdController,
    required this.licenseKeyDirController,
    required this.vendorSignKeyController,
    required this.vendorKeyDirController,
    required this.inspectPathController,
    required this.packageVersionController,
    required this.profileController,
    required this.remoteTargetController,
    required this.remoteStageController,
    required this.remoteRoutesController,
    required this.remotePortController,
    required this.manifestFilterController,
    required this.excludeController,
    required this.sources,
    required this.busy,
    required this.status,
    required this.rawContainers,
    required this.dryRun,
    required this.includeAssets,
    required this.obfuscate,
    required this.scanBeforeDump,
    required this.failOnScanWarning,
    required this.deployStartRunner,
    required this.onPickRoot,
    required this.onPickOutput,
    required this.onPickConfig,
    required this.onPickLicensePubkey,
    required this.onPickLicenseKeyDir,
    required this.onPickVendorSignKey,
    required this.onPickVendorKeyDir,
    required this.onPickInspectPath,
    required this.onAddFiles,
    required this.onAddFolder,
    required this.onRemoveSource,
    required this.onClearSources,
    required this.onGenerateKey,
    required this.onGenerateLicenseKeys,
    required this.onGenerateVendorKeys,
    required this.onDoctor,
    required this.onSelfTest,
    required this.onRotateKey,
    required this.onScan,
    required this.onDump,
    required this.onVerify,
    required this.onDecryptVerify,
    required this.onInspect,
    required this.onPackageSign,
    required this.onSaveProject,
    required this.onLoadProject,
    required this.onBuildLoader,
    required this.onInstallLoader,
    required this.onBuildAppImage,
    required this.onBuildDeb,
    required this.onBuildMac,
    required this.onBuildWindowsZip,
    required this.onBuildWindowsMsi,
    required this.onDeployStage,
    required this.onDeployCutover,
    required this.onDeployRollback,
    required this.onRawContainersChanged,
    required this.onDryRunChanged,
    required this.onIncludeAssetsChanged,
    required this.onObfuscateChanged,
    required this.onScanBeforeDumpChanged,
    required this.onFailOnScanWarningChanged,
    required this.onDeployStartRunnerChanged,
  });

  final TextEditingController rootController;
  final TextEditingController outputController;
  final TextEditingController keyController;
  final TextEditingController phpVersionController;
  final TextEditingController configController;
  final TextEditingController licensePubkeyController;
  final TextEditingController machineIdController;
  final TextEditingController licenseKeyDirController;
  final TextEditingController vendorSignKeyController;
  final TextEditingController vendorKeyDirController;
  final TextEditingController inspectPathController;
  final TextEditingController packageVersionController;
  final TextEditingController profileController;
  final TextEditingController remoteTargetController;
  final TextEditingController remoteStageController;
  final TextEditingController remoteRoutesController;
  final TextEditingController remotePortController;
  final TextEditingController manifestFilterController;
  final TextEditingController excludeController;
  final List<String> sources;
  final bool busy;
  final String? status;
  final bool rawContainers;
  final bool dryRun;
  final bool includeAssets;
  final bool obfuscate;
  final bool scanBeforeDump;
  final bool failOnScanWarning;
  final bool deployStartRunner;
  final VoidCallback onPickRoot;
  final VoidCallback onPickOutput;
  final VoidCallback onPickConfig;
  final VoidCallback onPickLicensePubkey;
  final VoidCallback onPickLicenseKeyDir;
  final VoidCallback onPickVendorSignKey;
  final VoidCallback onPickVendorKeyDir;
  final VoidCallback onPickInspectPath;
  final VoidCallback onAddFiles;
  final VoidCallback onAddFolder;
  final ValueChanged<String> onRemoveSource;
  final VoidCallback onClearSources;
  final VoidCallback onGenerateKey;
  final VoidCallback onGenerateLicenseKeys;
  final VoidCallback onGenerateVendorKeys;
  final VoidCallback onDoctor;
  final VoidCallback onSelfTest;
  final VoidCallback onRotateKey;
  final VoidCallback onScan;
  final VoidCallback onDump;
  final VoidCallback onVerify;
  final VoidCallback onDecryptVerify;
  final VoidCallback onInspect;
  final VoidCallback onPackageSign;
  final VoidCallback onSaveProject;
  final VoidCallback onLoadProject;
  final VoidCallback onBuildLoader;
  final VoidCallback onInstallLoader;
  final VoidCallback onBuildAppImage;
  final VoidCallback onBuildDeb;
  final VoidCallback onBuildMac;
  final VoidCallback onBuildWindowsZip;
  final VoidCallback onBuildWindowsMsi;
  final VoidCallback onDeployStage;
  final VoidCallback onDeployCutover;
  final VoidCallback onDeployRollback;
  final ValueChanged<bool> onRawContainersChanged;
  final ValueChanged<bool> onDryRunChanged;
  final ValueChanged<bool> onIncludeAssetsChanged;
  final ValueChanged<bool> onObfuscateChanged;
  final ValueChanged<bool> onScanBeforeDumpChanged;
  final ValueChanged<bool> onFailOnScanWarningChanged;
  final ValueChanged<bool> onDeployStartRunnerChanged;

  @override
  Widget build(BuildContext context) {
    return _Panel(
      child: SingleChildScrollView(
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
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: OutlinedButton.icon(
                    onPressed: busy ? null : onDecryptVerify,
                    icon: const Icon(Icons.lock),
                    label: const Text('Decrypt Test'),
                  ),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: OutlinedButton.icon(
                    onPressed: busy ? null : onPackageSign,
                    icon: const Icon(Icons.approval_outlined),
                    label: const Text('Sign Package'),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: TextField(
                    controller: phpVersionController,
                    decoration: const InputDecoration(
                      labelText: 'PHP version',
                      prefixIcon: Icon(Icons.terminal),
                    ),
                  ),
                ),
                const SizedBox(width: 10),
                IconButton.filledTonal(
                  tooltip: 'Doctor',
                  onPressed: busy ? null : onDoctor,
                  icon: const Icon(Icons.health_and_safety_outlined),
                ),
                const SizedBox(width: 8),
                IconButton.filledTonal(
                  tooltip: 'Self-test',
                  onPressed: busy ? null : onSelfTest,
                  icon: const Icon(Icons.checklist),
                ),
                const SizedBox(width: 8),
                IconButton.filledTonal(
                  tooltip: 'Build loader',
                  onPressed: busy ? null : onBuildLoader,
                  icon: const Icon(Icons.construction),
                ),
                const SizedBox(width: 8),
                IconButton.filled(
                  tooltip: 'Install Zend loader',
                  onPressed: busy ? null : onInstallLoader,
                  icon: const Icon(Icons.download_done),
                ),
              ],
            ),
            const SizedBox(height: 16),
            _OptionGrid(
              rawContainers: rawContainers,
              dryRun: dryRun,
              includeAssets: includeAssets,
              obfuscate: obfuscate,
              scanBeforeDump: scanBeforeDump,
              failOnScanWarning: failOnScanWarning,
              busy: busy,
              onRawContainersChanged: onRawContainersChanged,
              onDryRunChanged: onDryRunChanged,
              onIncludeAssetsChanged: onIncludeAssetsChanged,
              onObfuscateChanged: onObfuscateChanged,
              onScanBeforeDumpChanged: onScanBeforeDumpChanged,
              onFailOnScanWarningChanged: onFailOnScanWarningChanged,
            ),
            const SizedBox(height: 12),
            DropdownMenu<String>(
              controller: profileController,
              label: const Text('Profile'),
              leadingIcon: const Icon(Icons.account_tree),
              expandedInsets: EdgeInsets.zero,
              dropdownMenuEntries: const [
                DropdownMenuEntry(value: 'php-only', label: 'php-only'),
                DropdownMenuEntry(value: 'php-assets', label: 'php-assets'),
                DropdownMenuEntry(value: 'laravel', label: 'laravel'),
                DropdownMenuEntry(value: 'slim', label: 'slim'),
                DropdownMenuEntry(value: 'symfony', label: 'symfony'),
                DropdownMenuEntry(value: 'codeigniter', label: 'codeigniter'),
                DropdownMenuEntry(
                  value: 'wordpress-plugin',
                  label: 'wordpress-plugin',
                ),
                DropdownMenuEntry(
                  value: 'wordpress-theme',
                  label: 'wordpress-theme',
                ),
                DropdownMenuEntry(value: 'yii', label: 'yii'),
                DropdownMenuEntry(value: 'cakephp', label: 'cakephp'),
                DropdownMenuEntry(value: 'full-app', label: 'full-app'),
              ],
            ),
            const SizedBox(height: 8),
            Row(
              children: [
                Expanded(
                  child: OutlinedButton.icon(
                    onPressed: busy ? null : onSaveProject,
                    icon: const Icon(Icons.save_outlined),
                    label: const Text('Save Project'),
                  ),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: OutlinedButton.icon(
                    onPressed: busy ? null : onLoadProject,
                    icon: const Icon(Icons.folder_open),
                    label: const Text('Load Project'),
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),
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
            _PathField(
              label: 'bytecode.json',
              controller: configController,
              icon: Icons.rule_folder_outlined,
              onPick: busy ? null : onPickConfig,
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
              readOnly: true,
              enabled: !rawContainers,
              decoration: InputDecoration(
                labelText: rawContainers
                    ? 'Vendor secret disabled'
                    : 'Vendor secret key file',
                prefixIcon: const Icon(Icons.key),
                suffixIcon: IconButton(
                  tooltip: 'Regenerate vendor secret',
                  onPressed: busy || rawContainers ? null : onGenerateKey,
                  icon: const Icon(Icons.auto_awesome),
                ),
              ),
            ),
            const SizedBox(height: 8),
            OutlinedButton.icon(
              onPressed: busy || rawContainers ? null : onRotateKey,
              icon: const Icon(Icons.rotate_right),
              label: const Text('Rotate Key + Re-encode'),
            ),
            const SizedBox(height: 12),
            _PathField(
              label: 'License public key',
              controller: licensePubkeyController,
              icon: Icons.verified_user_outlined,
              onPick: busy || rawContainers ? null : onPickLicensePubkey,
            ),
            const SizedBox(height: 12),
            TextField(
              controller: machineIdController,
              enabled: !rawContainers,
              decoration: const InputDecoration(
                labelText: 'Authorized machine ID',
                prefixIcon: Icon(Icons.computer),
              ),
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: _PathField(
                    label: 'License key folder',
                    controller: licenseKeyDirController,
                    icon: Icons.vpn_key_outlined,
                    onPick: busy ? null : onPickLicenseKeyDir,
                  ),
                ),
                const SizedBox(width: 10),
                IconButton.filledTonal(
                  tooltip: 'Generate license keys',
                  onPressed: busy ? null : onGenerateLicenseKeys,
                  icon: const Icon(Icons.key_outlined),
                ),
              ],
            ),
            const SizedBox(height: 12),
            _PathField(
              label: 'Vendor sign key (Ed25519, seals builds)',
              controller: vendorSignKeyController,
              icon: Icons.gpp_good_outlined,
              onPick: busy || rawContainers ? null : onPickVendorSignKey,
            ),
            const SizedBox(height: 12),
            Row(
              children: [
                Expanded(
                  child: _PathField(
                    label: 'Vendor key folder',
                    controller: vendorKeyDirController,
                    icon: Icons.workspace_premium_outlined,
                    onPick: busy ? null : onPickVendorKeyDir,
                  ),
                ),
                const SizedBox(width: 10),
                IconButton.filledTonal(
                  tooltip:
                      'Generate vendor signing keys (prints compile-in hex to the log)',
                  onPressed: busy ? null : onGenerateVendorKeys,
                  icon: const Icon(Icons.enhanced_encryption_outlined),
                ),
              ],
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
                  flex: 2,
                  child: OutlinedButton.icon(
                    onPressed: busy ? null : onScan,
                    icon: const Icon(Icons.manage_search),
                    label: const Text('Scan'),
                  ),
                ),
                const SizedBox(width: 10),
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
            Row(
              children: [
                Expanded(
                  child: _PathField(
                    label: 'Inspect container',
                    controller: inspectPathController,
                    icon: Icons.pageview_outlined,
                    onPick: busy ? null : onPickInspectPath,
                  ),
                ),
                const SizedBox(width: 10),
                IconButton.filledTonal(
                  tooltip: 'Inspect BYTC',
                  onPressed: busy ? null : onInspect,
                  icon: const Icon(Icons.info_outline),
                ),
              ],
            ),
            const SizedBox(height: 16),
            _PackageActions(
              versionController: packageVersionController,
              busy: busy,
              onBuildAppImage: onBuildAppImage,
              onBuildDeb: onBuildDeb,
              onBuildMac: onBuildMac,
              onBuildWindowsZip: onBuildWindowsZip,
              onBuildWindowsMsi: onBuildWindowsMsi,
            ),
            const SizedBox(height: 12),
            _DeployActions(
              targetController: remoteTargetController,
              stageController: remoteStageController,
              routesController: remoteRoutesController,
              portController: remotePortController,
              startRunner: deployStartRunner,
              busy: busy,
              onDeployStage: onDeployStage,
              onDeployCutover: onDeployCutover,
              onDeployRollback: onDeployRollback,
              onStartRunnerChanged: onDeployStartRunnerChanged,
            ),
            const SizedBox(height: 12),
            _StatusBanner(status: status, busy: busy),
          ],
        ),
      ),
    );
  }
}

class _OptionGrid extends StatelessWidget {
  const _OptionGrid({
    required this.rawContainers,
    required this.dryRun,
    required this.includeAssets,
    required this.obfuscate,
    required this.scanBeforeDump,
    required this.failOnScanWarning,
    required this.busy,
    required this.onRawContainersChanged,
    required this.onDryRunChanged,
    required this.onIncludeAssetsChanged,
    required this.onObfuscateChanged,
    required this.onScanBeforeDumpChanged,
    required this.onFailOnScanWarningChanged,
  });

  final bool rawContainers;
  final bool dryRun;
  final bool includeAssets;
  final bool obfuscate;
  final bool scanBeforeDump;
  final bool failOnScanWarning;
  final bool busy;
  final ValueChanged<bool> onRawContainersChanged;
  final ValueChanged<bool> onDryRunChanged;
  final ValueChanged<bool> onIncludeAssetsChanged;
  final ValueChanged<bool> onObfuscateChanged;
  final ValueChanged<bool> onScanBeforeDumpChanged;
  final ValueChanged<bool> onFailOnScanWarningChanged;

  @override
  Widget build(BuildContext context) {
    return Wrap(
      spacing: 8,
      runSpacing: 8,
      children: [
        _OptionChip(
          icon: Icons.lock_open,
          label: 'Raw',
          selected: rawContainers,
          busy: busy,
          onChanged: onRawContainersChanged,
        ),
        _OptionChip(
          icon: Icons.fact_check_outlined,
          label: 'Dry run',
          selected: dryRun,
          busy: busy,
          onChanged: onDryRunChanged,
        ),
        _OptionChip(
          icon: Icons.web_asset,
          label: 'Assets',
          selected: includeAssets && !rawContainers,
          busy: busy || rawContainers,
          onChanged: onIncludeAssetsChanged,
        ),
        _OptionChip(
          icon: Icons.shuffle,
          label: 'Obfuscate',
          selected: obfuscate,
          busy: busy,
          onChanged: onObfuscateChanged,
        ),
        _OptionChip(
          icon: Icons.manage_search,
          label: 'Scan',
          selected: scanBeforeDump,
          busy: busy,
          onChanged: onScanBeforeDumpChanged,
        ),
        _OptionChip(
          icon: Icons.report_gmailerrorred,
          label: 'Fail warnings',
          selected: failOnScanWarning,
          busy: busy,
          onChanged: onFailOnScanWarningChanged,
        ),
      ],
    );
  }
}

class _OptionChip extends StatelessWidget {
  const _OptionChip({
    required this.icon,
    required this.label,
    required this.selected,
    required this.busy,
    required this.onChanged,
  });

  final IconData icon;
  final String label;
  final bool selected;
  final bool busy;
  final ValueChanged<bool> onChanged;

  @override
  Widget build(BuildContext context) {
    return FilterChip(
      avatar: Icon(icon, size: 17),
      label: Text(label),
      selected: selected,
      onSelected: busy ? null : onChanged,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
    );
  }
}

class _PackageActions extends StatelessWidget {
  const _PackageActions({
    required this.versionController,
    required this.busy,
    required this.onBuildAppImage,
    required this.onBuildDeb,
    required this.onBuildMac,
    required this.onBuildWindowsZip,
    required this.onBuildWindowsMsi,
  });

  final TextEditingController versionController;
  final bool busy;
  final VoidCallback onBuildAppImage;
  final VoidCallback onBuildDeb;
  final VoidCallback onBuildMac;
  final VoidCallback onBuildWindowsZip;
  final VoidCallback onBuildWindowsMsi;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: const Color(0xfffaf9f6),
        border: Border.all(color: const Color(0xffddd6c8)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              Expanded(
                child: TextField(
                  controller: versionController,
                  decoration: const InputDecoration(
                    labelText: 'Package version',
                    prefixIcon: Icon(Icons.sell_outlined),
                  ),
                ),
              ),
              const SizedBox(width: 10),
              IconButton.filledTonal(
                tooltip: 'Build AppImage',
                onPressed: busy ? null : onBuildAppImage,
                icon: const Icon(Icons.apps),
              ),
              const SizedBox(width: 8),
              IconButton.filledTonal(
                tooltip: 'Build deb',
                onPressed: busy ? null : onBuildDeb,
                icon: const Icon(Icons.inventory_2_outlined),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Row(
            children: [
              Expanded(
                child: OutlinedButton.icon(
                  onPressed: busy ? null : onBuildMac,
                  icon: const Icon(Icons.desktop_mac_outlined),
                  label: const Text('macOS'),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: OutlinedButton.icon(
                  onPressed: busy ? null : onBuildWindowsZip,
                  icon: const Icon(Icons.window),
                  label: const Text('Win ZIP'),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: OutlinedButton.icon(
                  onPressed: busy ? null : onBuildWindowsMsi,
                  icon: const Icon(Icons.install_desktop),
                  label: const Text('MSI'),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _DeployActions extends StatelessWidget {
  const _DeployActions({
    required this.targetController,
    required this.stageController,
    required this.routesController,
    required this.portController,
    required this.startRunner,
    required this.busy,
    required this.onDeployStage,
    required this.onDeployCutover,
    required this.onDeployRollback,
    required this.onStartRunnerChanged,
  });

  final TextEditingController targetController;
  final TextEditingController stageController;
  final TextEditingController routesController;
  final TextEditingController portController;
  final bool startRunner;
  final bool busy;
  final VoidCallback onDeployStage;
  final VoidCallback onDeployCutover;
  final VoidCallback onDeployRollback;
  final ValueChanged<bool> onStartRunnerChanged;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: const Color(0xfffaf9f6),
        border: Border.all(color: const Color(0xffddd6c8)),
        borderRadius: BorderRadius.circular(8),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          TextField(
            controller: targetController,
            decoration: const InputDecoration(
              labelText: 'Remote app',
              prefixIcon: Icon(Icons.dns_outlined),
            ),
          ),
          const SizedBox(height: 10),
          TextField(
            controller: stageController,
            decoration: const InputDecoration(
              labelText: 'Remote stage',
              prefixIcon: Icon(Icons.inventory_outlined),
            ),
          ),
          const SizedBox(height: 10),
          Material(
            color: Colors.transparent,
            child: SwitchListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              value: startRunner,
              onChanged: busy ? null : onStartRunnerChanged,
              title: const Text('Start staged runner'),
              secondary: const Icon(Icons.play_circle_outline),
            ),
          ),
          const SizedBox(height: 10),
          Row(
            children: [
              Expanded(
                flex: 3,
                child: TextField(
                  controller: routesController,
                  decoration: const InputDecoration(
                    labelText: 'Routes',
                    prefixIcon: Icon(Icons.route_outlined),
                  ),
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: TextField(
                  controller: portController,
                  decoration: const InputDecoration(
                    labelText: 'Port',
                    prefixIcon: Icon(Icons.tag),
                  ),
                ),
              ),
              const SizedBox(width: 10),
              IconButton.filledTonal(
                tooltip: 'Rollback latest backup',
                onPressed: busy ? null : onDeployRollback,
                icon: const Icon(Icons.restore),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Row(
            children: [
              Expanded(
                child: FilledButton.tonalIcon(
                  onPressed: busy ? null : onDeployStage,
                  icon: const Icon(Icons.cloud_upload_outlined),
                  label: const Text('Stage'),
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: FilledButton.icon(
                  onPressed: busy ? null : onDeployCutover,
                  icon: const Icon(Icons.swap_horiz),
                  label: const Text('Cutover'),
                ),
              ),
            ],
          ),
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
  const _ResultsPanel({
    required this.rows,
    required this.logText,
    required this.filterController,
  });

  final List<ManifestEntry> rows;
  final String logText;
  final TextEditingController filterController;

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
              TextField(
                controller: filterController,
                decoration: const InputDecoration(
                  labelText: 'Filter manifest',
                  prefixIcon: Icon(Icons.search),
                ),
              ),
              const SizedBox(height: 12),
              Expanded(
                child: stacked
                    ? Column(
                        children: [
                          Expanded(
                            child: _FilteredManifestTable(
                              rows: rows,
                              filterController: filterController,
                            ),
                          ),
                          const SizedBox(height: 12),
                          Expanded(child: _LogPane(text: logText)),
                        ],
                      )
                    : Row(
                        crossAxisAlignment: CrossAxisAlignment.stretch,
                        children: [
                          Expanded(
                            flex: 7,
                            child: _FilteredManifestTable(
                              rows: rows,
                              filterController: filterController,
                            ),
                          ),
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

class _FilteredManifestTable extends StatelessWidget {
  const _FilteredManifestTable({
    required this.rows,
    required this.filterController,
  });

  final List<ManifestEntry> rows;
  final TextEditingController filterController;

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: filterController,
      builder: (context, _) {
        final q = filterController.text.toLowerCase().trim();
        final filtered = q.isEmpty
            ? rows
            : rows
                  .where(
                    (row) =>
                        row.source.toLowerCase().contains(q) ||
                        row.output.toLowerCase().contains(q) ||
                        row.backend.toLowerCase().contains(q) ||
                        row.contentType.toLowerCase().contains(q),
                  )
                  .toList();
        return _ManifestTable(rows: filtered);
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
                      DataColumn(label: Text('Backend')),
                      DataColumn(label: Text('Type')),
                      DataColumn(label: Text('Size')),
                      DataColumn(label: Text('SHA-256')),
                    ],
                    rows: [
                      for (final row in rows)
                        DataRow(
                          cells: [
                            DataCell(SelectableText(row.source)),
                            DataCell(SelectableText(row.output)),
                            DataCell(Text(row.backend)),
                            DataCell(Text(row.contentType)),
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
