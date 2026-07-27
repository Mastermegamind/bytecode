#!/usr/bin/env bash
# Rung 12: prove the loader runs a realistic PSR / MVC / PDO application when
# every .php file is an encrypted BYTC container.
#
#   PSR : PSR-4 autoloading across nested namespaces + a PSR-3-style
#         LoggerInterface implemented by a concrete logger.
#   MVC : abstract Controller -> UserController, abstract BaseRepository ->
#         UserRepository, a User model; constructor DI, promoted properties,
#         parent::__construct().
#   PDO : new PDO(sqlite::memory:), ERRMODE_EXCEPTION, exec() DDL/DML,
#         prepare()+named params, fetchColumn(), and model hydration via
#         fetchObject(User::class) and fetchAll(PDO::FETCH_CLASS, User::class).
#
# The encoded output must match the plaintext baseline exactly.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
so="$root/php/src/modules/opdump.so"
php_bin="${PHP_BIN:-php}"
[ -f "$so" ] || { echo "FAIL: build php/src first ($so missing)"; exit 1; }

ext_dir="$("$php_bin" -r 'echo ini_get("extension_dir");')"
pdo_sqlite="$ext_dir/pdo_sqlite.so"
[ -f "$pdo_sqlite" ] || { echo "SKIP: pdo_sqlite not available"; exit 0; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
src="$work/src"
mkdir -p "$src/App/Model" "$src/App/Repo" "$src/App/Http" "$src/Psr/Log"

cat > "$src/autoload.php" <<'PHP'
<?php
spl_autoload_register(function (string $class) {
    $f = __DIR__ . '/' . str_replace('\\', '/', $class) . '.php';
    if (is_file($f)) require $f;
});
PHP
cat > "$src/Psr/Log/LoggerInterface.php" <<'PHP'
<?php
namespace Psr\Log;
interface LoggerInterface { public function info(string $message, array $context = []): void; }
PHP
cat > "$src/App/Http/ArrayLogger.php" <<'PHP'
<?php
namespace App\Http;
use Psr\Log\LoggerInterface;
final class ArrayLogger implements LoggerInterface {
    public array $lines = [];
    public function info(string $message, array $context = []): void { $this->lines[] = $message . ($context ? ' ' . json_encode($context) : ''); }
}
PHP
cat > "$src/App/Model/User.php" <<'PHP'
<?php
namespace App\Model;
class User { public int $id = 0; public string $name = ''; public string $role = 'user';
    public function label(): string { return "#{$this->id} {$this->name} ({$this->role})"; } }
PHP
cat > "$src/App/Repo/BaseRepository.php" <<'PHP'
<?php
namespace App\Repo;
abstract class BaseRepository {
    public function __construct(protected \PDO $pdo) {}
    abstract protected function table(): string;
    public function count(): int { return (int) $this->pdo->query('SELECT COUNT(*) FROM ' . $this->table())->fetchColumn(); }
}
PHP
cat > "$src/App/Repo/UserRepository.php" <<'PHP'
<?php
namespace App\Repo;
use App\Model\User;
final class UserRepository extends BaseRepository {
    protected function table(): string { return 'users'; }
    public function find(int $id): ?User {
        $st = $this->pdo->prepare('SELECT id, name, role FROM users WHERE id = :id');
        $st->execute(['id' => $id]);
        return $st->fetchObject(User::class) ?: null;
    }
    /** @return User[] */
    public function all(): array {
        return $this->pdo->query('SELECT id, name, role FROM users ORDER BY id')->fetchAll(\PDO::FETCH_CLASS, User::class);
    }
}
PHP
cat > "$src/App/Http/Controller.php" <<'PHP'
<?php
namespace App\Http;
abstract class Controller { public function __construct(protected \Psr\Log\LoggerInterface $log) {}
    protected function json(array $d): string { return json_encode($d); } }
PHP
cat > "$src/App/Http/UserController.php" <<'PHP'
<?php
namespace App\Http;
use App\Repo\UserRepository;
final class UserController extends Controller {
    public function __construct(\Psr\Log\LoggerInterface $log, private UserRepository $repo) { parent::__construct($log); }
    public function show(int $id): string {
        $u = $this->repo->find($id);
        $this->log->info('user.show', ['id' => $id, 'found' => (bool) $u]);
        return $this->json($u ? ['label' => $u->label()] : ['error' => 'not found']);
    }
    public function index(): string {
        $this->log->info('user.index');
        return $this->json(['count' => $this->repo->count(), 'users' => array_map(fn ($u) => $u->label(), $this->repo->all())]);
    }
}
PHP
cat > "$src/index.php" <<'PHP'
<?php
require __DIR__ . '/autoload.php';
$pdo = new PDO('sqlite::memory:');
$pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
$pdo->exec('CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, role TEXT)');
$pdo->exec("INSERT INTO users (name, role) VALUES ('Ada','admin'),('Grace','user'),('Linus','user')");
$log = new App\Http\ArrayLogger();
$ctl = new App\Http\UserController($log, new App\Repo\UserRepository($pdo));
echo $ctl->index(), PHP_EOL, $ctl->show(1), PHP_EOL, $ctl->show(999), PHP_EOL;
echo 'LOG: ', implode(' | ', $log->lines), PHP_EOL;
PHP

key="$("$php_bin" "$root/php/bin/bytecode-keygen")"

baseline="$("$php_bin" -n -d extension=pdo -d extension="$pdo_sqlite" "$src/index.php")"

out="$work/out"
BYTECODE_KEY="$key" PHP_BIN="$php_bin" "$php_bin" "$root/php/bin/bytecode-dump" "$src" "$out" >/dev/null
cp -r "$out"/. "$src"/   # drop-in: replace every .php with its container

encoded="$(BYTECODE_KEY="$key" "$php_bin" -n -d extension=pdo -d extension="$pdo_sqlite" -d zend_extension="$so" "$src/index.php" 2>/dev/null)"

if [ "$baseline" = "$encoded" ]; then
    echo "PASS: PSR/MVC/PDO encoded output matches plaintext"
    exit 0
fi
echo "FAIL: encoded output differs from baseline"
echo "--- baseline ---"; printf '%s\n' "$baseline"
echo "--- encoded  ---"; printf '%s\n' "$encoded"
exit 1
