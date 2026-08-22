<?php

/**
 * league/csv benchmark for the FastCSV-C suite.
 *
 * The library parses the CSV via PHP's C-backed fgetcsv/SplFileObject
 * (load phase), then queries the in-memory records with PHP (query phase),
 * matching how league/csv is typically used in a long-lived process.
 *
 * startup_ms = fixed cost of a trivial query on a 1-row CSV (autoload +
 *              library init + minimal query path).
 * load_ms    = parse the full CSV into memory, averaged over RUNS.
 * query_ms   = one in-process query over the loaded records, averaged.
 * total_ms   = load_ms + query_ms (one process that loads then queries).
 */

use League\Csv\Buffer;
use League\Csv\Reader;
use League\Csv\Statement;

ini_set('memory_limit', '2048M');

require __DIR__ . '/vendor/autoload.php';

$data = getenv('DATA') ?: '/data/data.csv';
$rows = (int) (getenv('ROWS') ?: 20000);
$runs = (int) (getenv('RUNS') ?: 3);
$out  = getenv('OUT') ?: '/results/league-csv.json';

if (!is_file($data)) {
    fwrite(STDERR, "data file not found: {$data}\n");
    exit(1);
}

function now_ms(): float
{
    return hrtime(true) / 1e6;
}

echo "league/csv bench: data={$data} rows={$rows} runs={$runs}\n";

// startup_ms: trivial query on a 1-row CSV, averaged over RUNS.
$tiny = tempnam(sys_get_temp_dir(), 'lcsv') . '.csv';
file_put_contents($tiny, "id,name\n1,foo\n");
$tinyReader = Reader::from($tiny);
$tinyReader->setHeaderOffset(0);
$tinyReader->setEscape('');
$startupTotal = 0.0;
for ($i = 0; $i < $runs; $i++) {
    $t0 = now_ms();
    $r = (new Statement())->where(static fn (array $row): bool => false)->process($tinyReader);
    foreach ($r as $_row) {
    }
    $startupTotal += now_ms() - $t0;
}
unlink($tiny);
$startupMs = $startupTotal / $runs;

$reader = Reader::from($data);
$reader->setHeaderOffset(0);
$reader->setEscape('');

// load_ms: materialize all records (C-core fgetcsv parsing), averaged.
$loadTotal = 0.0;
for ($i = 0; $i < $runs; $i++) {
    $t0 = now_ms();
    $records = [];
    foreach ($reader->getRecords() as $record) {
        $records[] = $record;
    }
    $loadTotal += now_ms() - $t0;
}
$loadMs = $loadTotal / $runs;

$buffer = new Buffer($reader->getHeader());
$buffer->insert(...$records);

$queries = [];

// q1: filter (where age >= 70)
$where = static function (array $row): bool {
    return (int) $row['age'] >= 70;
};
$q1Total = 0.0;
$q1Rows = 0;
for ($i = 0; $i < $runs; $i++) {
    $t0 = now_ms();
    $n = 0;
    $result = (new Statement())->where($where)->process($buffer);
    foreach ($result as $row) {
        $n++;
    }
    $q1Total += now_ms() - $t0;
    $q1Rows = $n;
}
$q1Ms = $q1Total / $runs;
$queries['q1_filter'] = ['total_ms' => round($loadMs + $q1Ms, 3), 'query_ms' => round($q1Ms, 3), 'rows' => $q1Rows];

// q2: group by city, count (PHP aggregation over parsed records)
$q2Total = 0.0;
$q2Rows = 0;
for ($i = 0; $i < $runs; $i++) {
    $t0 = now_ms();
    $counts = [];
    foreach ($records as $record) {
        $city = $record['city'];
        $counts[$city] = ($counts[$city] ?? 0) + 1;
    }
    $q2Total += now_ms() - $t0;
    $q2Rows = count($counts);
}
$q2Ms = $q2Total / $runs;
$queries['q2_group'] = ['total_ms' => round($loadMs + $q2Ms, 3), 'query_ms' => round($q2Ms, 3), 'rows' => $q2Rows];

// q3: order by salary desc, limit 100
$sort = static function (array $a, array $b): int {
    return (float) $b['salary'] <=> (float) $a['salary'];
};
$q3Total = 0.0;
$q3Rows = 0;
for ($i = 0; $i < $runs; $i++) {
    $t0 = now_ms();
    $result = (new Statement())
        ->orderBy($sort)
        ->limit(100)
        ->process($buffer);
    $n = 0;
    foreach ($result as $row) {
        $n++;
    }
    $q3Total += now_ms() - $t0;
    $q3Rows = $n;
}
$q3Ms = $q3Total / $runs;
$queries['q3_sort'] = ['total_ms' => round($loadMs + $q3Ms, 3), 'query_ms' => round($q3Ms, 3), 'rows' => $q3Rows];

// q4: count all records
$q4Total = 0.0;
$q4Rows = count($records);
for ($i = 0; $i < $runs; $i++) {
    $t0 = now_ms();
    $n = count($records);
    $q4Total += now_ms() - $t0;
}
$q4Ms = $q4Total / $runs;
$queries['q4_count'] = ['total_ms' => round($loadMs + $q4Ms, 3), 'query_ms' => round($q4Ms, 3), 'rows' => $q4Rows];

$payload = [
    'candidate' => 'league-csv',
    'rows' => $rows,
    'runs' => $runs,
    'startup_ms' => round($startupMs, 3),
    'load_ms' => round($loadMs, 3),
    'queries' => $queries,
];

file_put_contents($out, json_encode($payload, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES));
echo "wrote {$out}\n";
echo json_encode($payload, JSON_PRETTY_PRINT) . "\n";
