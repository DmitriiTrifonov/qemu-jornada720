<?php
/*
 * Router for the PHP server run.sh starts next to the emulator: /files/
 * lists and hands out the host folder J720_FILES (default ~/jornada-files)
 * so Pocket IE can download programs and documents into CE; everything
 * else goes to FrogFind in the document root.
 */
$uri = rawurldecode(parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH));

if ($uri === '/files' ||
    ($uri === '/' && !is_file($_SERVER['DOCUMENT_ROOT'] . '/index.php'))) {
    header('Location: /files/');
    return true;
}
if (strncmp($uri, '/files/', 7) !== 0) {
    return false;   /* FrogFind */
}

$root = realpath(getenv('J720_FILES') ?: getenv('HOME') . '/jornada-files');
$path = $root === false ? false : realpath($root . '/' . substr($uri, 7));

/* stay inside the shared folder */
if ($path === false || ($path !== $root && strncmp($path, $root . '/', strlen($root) + 1) !== 0)) {
    http_response_code(404);
    echo "<html><body>Not found</body></html>";
    return true;
}

if (is_file($path)) {
    header('Content-Type: application/octet-stream');
    header('Content-Length: ' . filesize($path));
    header('Content-Disposition: attachment; filename="' . basename($path) . '"');
    readfile($path);
    return true;
}

$rel = substr($path, strlen($root));
$names = array_diff(scandir($path), ['.', '..']);
header('Content-Type: text/html; charset=utf-8');
echo '<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 2.0//EN">' . "\n";
echo "<html><head><title>Files" . htmlspecialchars($rel) . "/</title></head><body>\n";
echo "<h2>Files" . htmlspecialchars($rel) . "/</h2>\n";
if ($rel !== '') {
    echo "<a href=\"../\">..</a><br>\n";
}
if (!$names) {
    echo "<p>Empty. Put files into " . htmlspecialchars($path) . " on the phone.</p>\n";
}
foreach ($names as $name) {
    $full = "$path/$name";
    $dir = is_dir($full);
    $href = rawurlencode($name) . ($dir ? '/' : '');
    $size = $dir ? '' : ' (' . max(1, (int)ceil(filesize($full) / 1024)) . ' KB)';
    echo "<a href=\"$href\">" . htmlspecialchars($name) . ($dir ? '/' : '') . "</a>$size<br>\n";
}
echo "</body></html>\n";
return true;
