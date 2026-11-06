<?php

declare(strict_types=1);

/*
 * Demo source for exercising Bytecode encoding + obfuscation.
 *
 * It deliberately mixes constructs so you can see each feature:
 *   - plain local variables           -> renamed by --obfuscate
 *   - a typed property with a docblock -> doc-comment stripped in bytecode
 *   - a function using compact()       -> obfuscation safely SKIPS it
 *   - a variable-variable              -> flagged by `scan`, skipped by obfuscate
 *
 * Behaviour is unchanged by encoding/obfuscation; run it before and after to
 * confirm identical output.
 */

namespace Demo;

final class Invoice
{
    /** The customer this invoice belongs to (doc-comment is stripped in bytecode). */
    public string $customer;

    /** @var array<int, float> line item amounts */
    private array $items = [];

    public function __construct(string $customer)
    {
        $this->customer = $customer;
    }

    public function addItem(float $amount): void
    {
        // `$normalized` is a non-parameter local -> renamed by --obfuscate.
        $_v0 = round($amount, 2);
        $this->items[] = $_v0;
    }

    public function total(): float
    {
        // `$running` and `$value` are locals -> renamed by --obfuscate.
        $_v0 = 0.0;
        foreach ($this->items as $_v1) {
            $_v0 += $_v1;
        }
        return round($_v0, 2);
    }
}

/**
 * Uses compact(), so the obfuscator fails closed and leaves this function's
 * locals untouched (renaming them would break compact()).
 */
function summarize(string $customer, float $total): array
{
    $status = $total > 0 ? 'due' : 'paid';
    return compact('customer', 'total', 'status');
}

/**
 * Contains a variable-variable: `bytecode scan` warns about this, and the
 * obfuscator skips the function rather than risk changing behaviour.
 */
function dynamicLookup(array $row, string $field): mixed
{
    $$field = $row[$field] ?? null;
    return $$field;
}

// ---- driver (so the file is runnable before and after encoding) -----------

$invoice = new Invoice('Acme Corp');
$invoice->addItem(19.995);
$invoice->addItem(5.00);
$invoice->addItem(100.251);

$summary = summarize($invoice->customer, $invoice->total());
echo $summary['customer'], ': ', number_format($summary['total'], 2),
    ' (', $summary['status'], ")\n";
echo 'lookup: ', (string) dynamicLookup(['sku' => 'A-1'], 'sku'), "\n";
