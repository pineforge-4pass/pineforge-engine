"""Exercise checkpoint diagnostics, optional writes and full verification through the CLI."""
from contextlib import closing, nullcontext
import fcntl
import hashlib
import json
from pathlib import Path
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time


def equal(actual, expected):
    assert actual == expected, {"actual": actual, "expected": expected}


def invoke(arguments, code=0):
    result = subprocess.run(arguments, text=True, capture_output=True, timeout=30)
    assert result.returncode == code, (arguments, result.returncode, code, result.stdout, result.stderr)
    return result


def query(path, sql):
    with sqlite3.connect(path) as database:
        return database.execute(sql).fetchone()[0]


def execute(path, sql):
    with sqlite3.connect(path) as database:
        database.executescript(sql)


def ledger_image(path, immutable=False):
    # Compare the actual source schema and every ordered row, independently
    # of any producer manifest. immutable also proves the copy stands alone.
    uri = path.resolve().as_uri() + "?mode=ro" + ("&immutable=1" if immutable else "")
    with closing(sqlite3.connect(uri, uri=True)) as database:
        database.execute("BEGIN")
        equal(database.execute("PRAGMA integrity_check").fetchall(), [("ok",)])
        equal(database.execute("PRAGMA foreign_key_check").fetchall(), [])
        schema = database.execute("SELECT type,name,tbl_name,sql FROM sqlite_master "
                                  "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name").fetchall()
        tables = database.execute("SELECT name FROM sqlite_master WHERE type='table' "
                                  "AND name NOT LIKE 'sqlite_%' ORDER BY name").fetchall()
        rows = {name: database.execute('SELECT * FROM "' + name.replace('"', '""') +
                                      '" ORDER BY rowid').fetchall() for (name,) in tables}
        return schema, rows


def pre_upgrade_backup(ledger, expected):
    # The contract leaves the digest-based filename spelling open. Only
    # inspect this fixture's ledger directory, and identify SQLite images by
    # their file header rather than assuming a new CLI or manifest schema.
    copies = []
    for candidate in sorted(ledger.parent.rglob("*")):
        if candidate == ledger or not candidate.is_file():
            continue
        with candidate.open("rb") as source:
            if source.read(16) == b"SQLite format 3\0":
                copies.append(candidate)
    assert copies, ("automatic pre-upgrade SQLite backup missing", str(ledger))
    equal(len(copies), 1)
    backup = copies[0]
    assert not backup.is_symlink(), backup
    equal(backup.stat().st_mode & 0o777, 0o600)
    equal(ledger_image(backup, immutable=True), expected)
    return backup


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(65536), b""):
            digest.update(block)
    return digest.hexdigest()


def backup_source(ledger):
    # Independent source oracle for the documented v1 digest encoding, in
    # addition to ledger_image's schema and actual-row equality assertion.
    def unsigned(value):
        return struct.pack(">Q", value & ((1 << 64) - 1))

    def text(digest, value):
        data = value.encode("utf-8")
        digest.update(unsigned(len(data)) + data)

    def row(digest, values):
        digest.update(b"\x01")
        for value in values:
            if value is None:
                digest.update(b"\x05")
            elif isinstance(value, int):
                digest.update(b"\x01" + unsigned(value))
            elif isinstance(value, float):
                digest.update(b"\x02" + struct.pack(">d", value))
            else:
                data = value.encode("utf-8") if isinstance(value, str) else value
                digest.update((b"\x03" if isinstance(value, str) else b"\x04") +
                              unsigned(len(data)) + data)

    with closing(sqlite3.connect(ledger.resolve().as_uri() + "?mode=ro", uri=True)) as database:
        database.execute("BEGIN")
        schema = hashlib.sha256()
        text(schema, "pineforge-ledger-schema/v1")
        for values in database.execute("SELECT type,name,tbl_name,sql FROM sqlite_master "
                                       "ORDER BY type COLLATE BINARY,name COLLATE BINARY"):
            row(schema, values)
        schema.update(b"\x00")
        rows = hashlib.sha256()
        text(rows, "pineforge-ledger-rows/v1")
        tables = [name for (name,) in database.execute(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name COLLATE BINARY")]
        for name in tables:
            quoted = '"' + name.replace('"', '""') + '"'
            columns = [col[0] for col in database.execute("SELECT * FROM " + quoted + " LIMIT 0").description]
            text(rows, name)
            rows.update(unsigned(len(columns)))
            for column in columns:
                text(rows, column)
            order = ",".join(f"{index + 1} COLLATE BINARY" for index in range(len(columns)))
            for values in database.execute("SELECT * FROM " + quoted + " ORDER BY " + order):
                row(rows, values)
            rows.update(b"\x00")

        def scalar(sql):
            return database.execute(sql).fetchone()[0]

        version, identity = database.execute(
            "SELECT schema_version,identity FROM metadata WHERE singleton=1").fetchone()
        status = ledger.stat()
        source = {
            "path": str(ledger.resolve()), "device": status.st_dev, "inode": status.st_ino,
            "owner": status.st_uid, "layout": tables, "identity": identity, "schema_version": version,
            "sqlite_schema_cookie": scalar("PRAGMA schema_version"),
            "sqlite_user_version": scalar("PRAGMA user_version"),
            "sqlite_application_id": scalar("PRAGMA application_id"),
            "page_size": scalar("PRAGMA page_size"),
            "input_cursor": scalar("SELECT COALESCE(MAX(input_index)+1,0) FROM inputs"),
            "event_cursor": scalar("SELECT COALESCE(MAX(ordinal),0) FROM events"),
            "report_cursor": scalar("SELECT COALESCE(MAX(input_cursor),0) FROM report_snapshots"),
            "schema_sha256": schema.hexdigest(), "rows_sha256": rows.hexdigest(),
        }
    sizes = {key: path.stat().st_size if path.exists() else 0 for key, path in
             (("main", ledger), ("wal", Path(str(ledger) + "-wal")),
              ("shm", Path(str(ledger) + "-shm")))}
    return source, sizes


def backup_manifest(ledger, image, expected_source, expected_sizes):
    directory = ledger.with_name(ledger.name + ".backups")
    equal(image.parent, directory)
    equal(directory.stat().st_mode & 0o777, 0o700)
    manifest_path = image.with_name(image.name + ".manifest.json")
    assert not manifest_path.is_symlink(), manifest_path
    equal(manifest_path.stat().st_mode & 0o777, 0o600)
    manifest = json.loads(manifest_path.read_text())
    equal(manifest["schema"], "pineforge-ledger-backup/v1")
    equal(manifest["complete"], True)
    equal(manifest["source"], expected_source)
    equal(manifest["original_sizes"], expected_sizes)
    equal(manifest["image"], image.name)
    equal(manifest["image_bytes"], image.stat().st_size)
    equal(manifest["image_sha256"], file_sha256(image))
    encoded = json.dumps(expected_source, sort_keys=True, separators=(",", ":"),
                         ensure_ascii=False).encode("utf-8")
    prefix_digest = hashlib.sha256(encoded).hexdigest()
    equal(manifest["source_prefix_sha256"], prefix_digest)
    assert image.name.startswith(ledger.name + "." + prefix_digest + "."), image.name
    return manifest_path


def backup_failure(result, reason):
    prefix = "pineforge-live: "
    failures = [json.loads(line[len(prefix):]) for line in result.stderr.splitlines()
                if line.startswith(prefix + '{"args":')]
    equal(len(failures), 1)
    failure = failures[0]
    equal(failure["code"], "live_ledger_backup_failed")
    equal(failure["class"], "resource")
    equal(failure["retryable"], False)
    equal(failure["args"], {"reason": reason})
    equal(result.stdout, "")


def diagnostic_lines(errors):
    prefix = "pineforge-live: "
    return [json.loads(line[len(prefix):])["checkpoint_diagnostic"]
            for line in errors.splitlines()
            if line.startswith(prefix + '{"checkpoint_diagnostic":')]


def write_diagnostic(actual, reason, budget=0, build=0, io=0):
    equal(actual["code"], "write_failed")
    equal(actual["reason"], reason)
    equal(actual["budget_count"], budget)
    equal(actual["build_count"], build)
    equal(actual["io_count"], io)
    equal(actual["count"], budget + build + io)
    equal(actual["count"], sum(actual[key] for key in ("budget_count", "build_count", "io_count")))


def main():
    runner, library = sys.argv[1:]
    children = []
    # Preserve the source image and child output for the first red handoff.
    with nullcontext(tempfile.mkdtemp(prefix="pineforge-checkpoint-cli-")) as directory:
        root = Path(directory)
        print("artifacts:", root, flush=True)
        warmup = root / "warmup.csv"
        warmup.write_text("timestamp,open,high,low,close,volume\n" +
                          "".join(f"{index * 60000},100,102,99,101,4\n" for index in range(3)))
        config = root / "native.json"
        config.write_text(json.dumps({
            "run": {"session_key": "checkpoint-test", "run_number": 1},
            "clock": {"input_tf": "1", "script_tf": "1", "timezone": "UTC",
                      "session": "24x7", "chart_timezone": ""},
            "instrument": {"ticker": "MOCK", "tickerid": "TEST:MOCK", "type": "crypto",
                           "currency": "USDT", "basecurrency": "ETH", "description": "",
                           "volumetype": ""},
            "execution": {"initial_capital": 10000, "point_value": 1, "account_fx": 1,
                          "price_tick": 0.01, "slippage_ticks": 0, "fee_kind": "CashPerExecution",
                          "fee_value": 6, "quantity_grid": None, "close_execution": "NextEligiblePoint",
                          "max_abs_units": None, "max_open_lots": None,
                          "allowed_open_directions": 3, "initial_margin_fraction": None},
        }))
        events = [dict(type="bar", bar=dict(ts_open=index * 60000, o=100, h=102, l=99, c=101, v=4))
                  for index in range(3, 10)]
        feed = root / "feed.jsonl"
        feed.write_text("".join(json.dumps(event) + "\n" for event in events))

        def command(ledger, source=feed, interval=2):
            return [runner, "run", "--strategy", library, "--warmup", str(warmup),
                    "--native-config", str(config), "--mode", "bars", "--ledger", str(ledger),
                    "--feed", str(source), "--checkpoint-interval", str(interval)]

        def report(ledger, *options, code=0):
            return invoke([runner, "report", "--ledger", str(ledger), *options], code)

        def seed(name):
            ledger = root / (name + ".sqlite")
            equal(json.loads(invoke(command(ledger)).stdout)["inputs_committed"], len(events))
            equal(query(ledger, "SELECT group_concat(input_cursor,',') FROM "
                               "(SELECT input_cursor FROM report_checkpoints ORDER BY input_cursor)"), "2,4")
            return ledger

        def wait_for(process, predicate, label):
            deadline = time.monotonic() + 20
            last = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    output, errors = process.communicate()
                    raise AssertionError((label, process.returncode, output, errors))
                try:
                    last = predicate()
                    if last:
                        return last
                except (FileNotFoundError, json.JSONDecodeError, sqlite3.OperationalError) as error:
                    last = str(error)
                time.sleep(0.02)
            raise AssertionError((label, "condition not reached", last))

        def start(ledger, count, name):
            health = root / (name + ".status.json")
            process = subprocess.Popen(command(ledger, "-") +
                                       ["--from-input", str(count), "--status-file", str(health),
                                        "--feed-idle-timeout", "60", "--feed-message-timeout", "60"],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True)
            children.append(process)
            wait_for(process, lambda: json.loads(health.read_text()).get("state") == "running",
                     "service running")
            return process, health

        def finish(process, artifacts=None):
            output, errors = process.communicate(timeout=30)
            if artifacts is not None:
                artifacts.with_suffix(".command.json").write_text(json.dumps(process.args) + "\n")
                artifacts.with_suffix(".stdout").write_text(output)
                artifacts.with_suffix(".stderr").write_text(errors)
                artifacts.with_suffix(".exit").write_text(str(process.returncode) + "\n")
            equal(process.returncode, 0)
            return json.loads(output), errors

        def send(process, event):
            process.stdin.write(json.dumps(event) + "\n")
            process.stdin.flush()

        try:
            ledger = seed("full-verification")
            expected = report(ledger, "--at-input", "4").stdout
            equal(report(ledger, "--verify-full", "--at-input", "4").stdout, expected)
            equal(report(ledger, "--verify-full").stdout, report(ledger).stdout)
            execute(ledger, "UPDATE inputs SET state_hash='changed' WHERE input_index=0;")
            equal(report(ledger, "--at-input", "4").stdout, expected)
            refused = report(ledger, "--verify-full", "--at-input", "4", code=1)
            equal(refused.stdout, "")
            assert "native ledger report integrity mismatch" in refused.stderr, refused.stderr
            print("PASS full verification reaches the chain below the checkpoint", flush=True)

            ledger = seed("startup-diagnostic")
            expected = report(ledger).stdout
            execute(ledger, "DROP TRIGGER report_checkpoint_no_update;"
                            "UPDATE report_checkpoints SET payload=payload || ' ' WHERE input_cursor=4;")
            process, health = start(ledger, len(events), "startup-diagnostic")
            status = json.loads(health.read_text())
            equal(status["checkpoint_diagnostic"]["code"], "payload_hash_mismatch")
            equal(status["checkpoint_diagnostic"]["cursor"], 4)
            equal(status["checkpoint_diagnostic"]["reason"], None)
            equal([status["checkpoint_diagnostic"][key] for key in
                   ("count", "budget_count", "build_count", "io_count")], [0, 0, 0, 0])
            equal(status["ready"], True)
            equal(report(ledger).stdout, expected)
            summary, errors = finish(process)
            equal(summary["inputs_processed"], 0)
            equal([(item["code"], item["cursor"]) for item in diagnostic_lines(errors)],
                  [("payload_hash_mismatch", 4)])
            equal(json.loads(health.read_text())["checkpoint_diagnostic"], status["checkpoint_diagnostic"])
            print("PASS startup fallback publishes its diagnostic to stderr and service status", flush=True)

            ledger = seed("consistent-edit")
            expected = report(ledger, "--at-input", "4").stdout
            payload = query(ledger, "SELECT payload FROM report_checkpoints WHERE input_cursor=4")
            wrong = payload.replace('"input_cursor":4', '"input_cursor":3', 1)
            assert wrong != payload, (payload, wrong)
            with sqlite3.connect(ledger) as database:
                database.execute("DROP TRIGGER report_checkpoint_no_update")
                changed = database.execute("UPDATE report_checkpoints SET payload=?,payload_hash=? "
                                           "WHERE input_cursor=4",
                                           (wrong, hashlib.sha256(wrong.encode()).hexdigest()))
                equal(changed.rowcount, 1)
            equal(report(ledger, "--at-input", "4").stdout, wrong + "\n")
            equal(report(ledger, "--verify-full", "--at-input", "4").stdout, expected)
            refused = invoke(command(ledger), 1)
            assert "native replay report checkpoint mismatch" in refused.stderr, refused.stderr
            equal(query(ledger, "SELECT COUNT(*) FROM inputs"), len(events))
            print("PASS a consistently edited checkpoint is refused by replay equality", flush=True)

            ledger = root / "optional-write.sqlite"
            process, health = start(ledger, 0, "optional-write")
            execute(ledger, "CREATE TRIGGER reject_report_checkpoint BEFORE INSERT ON report_checkpoints "
                            "BEGIN SELECT RAISE(ABORT,'checkpoint test'); END;")
            send(process, events[0])
            send(process, events[1])
            wait_for(process, lambda: (
                json.loads(health.read_text()).get("checkpoint_diagnostic") or {}).get("cursor") == 2,
                "checkpoint write diagnostic")
            status = json.loads(health.read_text())
            equal(status["checkpoint_diagnostic"]["code"], "write_failed")
            equal(status["checkpoint_diagnostic"]["count"], 1)
            write_diagnostic(status["checkpoint_diagnostic"], "io", io=1)
            equal(status["state"], "running")
            equal(status["ready"], True)
            equal(query(ledger, "SELECT COUNT(*) FROM inputs"), 2)
            equal(query(ledger, "SELECT COUNT(*) FROM report_snapshots WHERE input_cursor=2"), 1)
            equal(query(ledger, "SELECT COUNT(*) FROM report_integrity WHERE input_cursor=2"), 1)
            equal(query(ledger, "SELECT COUNT(*) FROM report_checkpoints"), 0)
            send(process, events[2])
            wait_for(process, lambda: query(ledger, "SELECT COUNT(*) FROM inputs") == 3,
                     "next input commits after checkpoint failure")
            execute(ledger, "DROP TRIGGER reject_report_checkpoint;")
            send(process, events[3])
            wait_for(process, lambda: query(ledger, "SELECT MAX(input_cursor) FROM report_checkpoints") == 4,
                     "next geometric checkpoint succeeds")
            equal(report(ledger).stdout, report(ledger, "--verify-full").stdout)
            summary, errors = finish(process)
            equal(summary["inputs_committed"], 4)
            equal(summary["inputs_processed"], 4)
            equal([(item["code"], item["cursor"]) for item in diagnostic_lines(errors)],
                  [("write_failed", 2)])
            equal([item["count"] for item in diagnostic_lines(errors)], [1])
            equal(diagnostic_lines(errors), [status["checkpoint_diagnostic"]])
            equal(json.loads(health.read_text())["checkpoint_diagnostic"], status["checkpoint_diagnostic"])
            print("PASS a failed checkpoint leaves its input durable and the service running", flush=True)

            upgrade = root / "upgrade"
            upgrade.mkdir()
            ledger = upgrade / "ledger.sqlite"
            short_feed = root / "short.jsonl"
            short_feed.write_text("".join(json.dumps(event) + "\n" for event in events[:3]))
            invoke(command(ledger, short_feed, interval=100))
            expected = report(ledger, "--verify-full").stdout
            (root / "upgrade-before.report.json").write_text(expected)
            # Pin the checkpointed 13-table main image, then commit the real
            # historical 12-table layout to WAL. A main-file-only copy has
            # the wrong schema even though all its original inputs are valid.
            with closing(sqlite3.connect(ledger)) as writer, closing(sqlite3.connect(
                    ledger.resolve().as_uri() + "?mode=ro", uri=True)) as pinned:
                equal(writer.execute("PRAGMA wal_checkpoint(TRUNCATE)").fetchone()[0], 0)
                writer.execute("PRAGMA wal_autocheckpoint=0")
                pinned.execute("BEGIN")
                equal(pinned.execute("SELECT COUNT(*) FROM sqlite_master "
                                     "WHERE type='table'").fetchone()[0], 13)
                writer.execute("DROP TABLE report_checkpoints")
                writer.commit()
                before = ledger_image(ledger)
                equal(len(before[1]), 12)
                assert "report_checkpoints" not in before[1], before[1].keys()
                equal(len(before[1]["inputs"]), 3)
                assert before[1]["events"], "backup fixture must include real strategy actions"
                equal(sorted(row[0] for row in before[1]["report_snapshots"]), [0, 1, 2, 3])
                assert Path(str(ledger) + "-wal").stat().st_size > 32
                equal(len(ledger_image(ledger, immutable=True)[1]), 13)
                (root / "upgrade-before.json").write_text(json.dumps(before, indent=2) + "\n")
                equal(report(ledger, "--verify-full").stdout, expected)
                print("backup fixture: 3 inputs, nonempty actions, 12 committed tables; "
                      "main-only image has 13 tables", flush=True)
                source_before, sizes_before = backup_source(ledger)
                process, health = start(ledger, 3, "upgrade")
                equal(query(ledger, "SELECT COUNT(*) FROM inputs"), 3)
                equal(query(ledger, "SELECT COUNT(*) FROM report_checkpoints"), 1)
                equal(query(ledger, "SELECT MAX(input_cursor) FROM report_checkpoints"), 3)
                equal(report(ledger).stdout, expected)
                equal(json.loads(health.read_text())["checkpoint_diagnostic"], None)
                finish(process, root / "upgrade-run")
                backup = pre_upgrade_backup(ledger, before)
                backup_manifest(ledger, backup, source_before, sizes_before)
                equal(report(backup, "--verify-full").stdout, expected)
            print("PASS upgrade checkpoints at open before any new input", flush=True)
            print("PASS automatic backup independently preserves the committed pre-upgrade WAL image",
                  flush=True)

            # A complete current ledger, including a fresh one, must not
            # accumulate mandatory copies on ordinary repeated opens.
            current = root / "current.sqlite"
            invoke(command(current, short_feed, interval=100))
            invoke(command(current, short_feed, interval=100))
            assert not current.with_name(current.name + ".backups").exists(), current
            # Index-only upgrade: unchanged table/column counts still need a
            # copy. A leftover partial and unrelated file must survive.
            execute(current, "DROP INDEX delivery_results;")
            current_before = ledger_image(current)
            directory = current.with_name(current.name + ".backups")
            directory.mkdir(mode=0o700)
            partial = directory / "retained.sqlite.partial"
            with closing(sqlite3.connect(current)) as source, closing(sqlite3.connect(partial)) as target:
                source.backup(target)
            partial.chmod(0o600)
            retained_hash = file_sha256(partial)
            unrelated = directory / "operator-note"
            unrelated.write_text("retain this unrelated recovery note\n")
            with closing(sqlite3.connect(current.resolve().as_uri() + "?mode=ro", uri=True)) as reader:
                reader.execute("BEGIN")
                reader.execute("SELECT COUNT(*) FROM sqlite_master").fetchone()
                current_source, current_sizes = backup_source(current)
                invoke(command(current, short_feed, interval=100))
            completions = list(directory.glob("*.sqlite.manifest.json"))
            equal(len(completions), 1)
            image = directory / json.loads(completions[0].read_text())["image"]
            equal(ledger_image(image, immutable=True), current_before)
            backup_manifest(current, image, current_source, current_sizes)
            equal(file_sha256(partial), retained_hash)
            equal(unrelated.read_text(), "retain this unrelated recovery note\n")
            retained = {path.name: file_sha256(path) for path in directory.iterdir() if path.is_file()}
            invoke(command(current, short_feed, interval=100))
            equal({path.name: file_sha256(path) for path in directory.iterdir() if path.is_file()}, retained)
            print("PASS index-only upgrade backs up once and preserves partial and unrelated files",
                  flush=True)

            blocked = root / "backup-io.sqlite"
            invoke(command(blocked, short_feed, interval=100))
            execute(blocked, "DROP TABLE report_checkpoints;")
            blocked_before = ledger_image(blocked)
            obstruction = blocked.with_name(blocked.name + ".backups")
            obstruction.write_text("operator-owned file\n")
            backup_failure(invoke(command(blocked, short_feed, interval=100), code=1), "io")
            equal(ledger_image(blocked), blocked_before)
            equal(obstruction.read_text(), "operator-owned file\n")
            print("PASS backup destination failure refuses before schema work", flush=True)

            invalid = root / "backup-verification.sqlite"
            invoke(command(invalid, short_feed, interval=100))
            execute(invalid, "DROP TABLE report_checkpoints; UPDATE events SET "
                    "input_index=(SELECT MAX(input_index)+2 FROM inputs);")
            invalid_before = backup_source(invalid)
            backup_failure(invoke(command(invalid, short_feed, interval=100), code=1), "verification")
            equal(backup_source(invalid)[0], invalid_before[0])
            equal(query(invalid, "SELECT COUNT(*) FROM sqlite_master WHERE name='report_checkpoints'"), 0)
            print("PASS source foreign-key failure refuses before schema work", flush=True)

            cancelled = root / "backup-cancelled.sqlite"
            invoke(command(cancelled, short_feed, interval=100))
            execute(cancelled, "DROP TABLE report_checkpoints; PRAGMA journal_mode=DELETE;")
            cancelled_before = ledger_image(cancelled)
            with closing(sqlite3.connect(cancelled)) as writer, \
                    Path(str(cancelled) + ".lock").open("rb") as runner_lock:
                writer.execute("BEGIN EXCLUSIVE")
                process = subprocess.Popen(command(cancelled, short_feed, interval=100),
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                children.append(process)

                def runner_has_lock():
                    try:
                        fcntl.flock(runner_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        return True
                    fcntl.flock(runner_lock, fcntl.LOCK_UN)
                    return False

                wait_for(process, runner_has_lock, "runner owns lock during blocked read-only inspection")
                process.send_signal(signal.SIGTERM)
                output, errors = process.communicate(timeout=30)
                equal(process.returncode, 1)
                backup_failure(subprocess.CompletedProcess(process.args, process.returncode, output, errors),
                               "incomplete")
                writer.rollback()
            equal(ledger_image(cancelled), cancelled_before)
            print("PASS cancellation of blocked backup inspection preserves the old source", flush=True)

            # An ordinary SQLite reader can postpone the existing DELETE->WAL
            # switch AFTER backup publication. Retry the same source to cover
            # completed-copy reuse and rejection of an actually damaged image.
            for damage in ("none", "file-hash", "source-rows"):
                retained_ledger = root / ("backup-" + damage + ".sqlite")
                invoke(command(retained_ledger, short_feed, interval=100))
                execute(retained_ledger, "DROP TABLE report_checkpoints; PRAGMA journal_mode=DELETE;")
                retained_before = ledger_image(retained_ledger)
                retained_source, retained_sizes = backup_source(retained_ledger)
                directory = retained_ledger.with_name(retained_ledger.name + ".backups")
                with closing(sqlite3.connect(retained_ledger.resolve().as_uri() + "?mode=ro",
                                             uri=True)) as reader:
                    reader.execute("BEGIN")
                    reader.execute("SELECT COUNT(*) FROM sqlite_master").fetchone()
                    refused = invoke(command(retained_ledger, short_feed, interval=100), code=1)
                    assert "native ledger database operation failed" in refused.stderr, refused.stderr
                    equal(ledger_image(retained_ledger), retained_before)
                    completions = list(directory.glob("*.sqlite.manifest.json"))
                    equal(len(completions), 1)
                    first_manifest = completions[0]
                    first_image = directory / json.loads(first_manifest.read_text())["image"]
                    backup_manifest(retained_ledger, first_image, retained_source, retained_sizes)
                if damage == "file-hash":
                    with first_image.open("r+b") as image_file:
                        # Damage real bytes without rewriting the producer's
                        # expected hash. This image must never be reused.
                        image_file.seek(first_image.stat().st_size - 1)
                        byte = image_file.read(1)
                        image_file.seek(-1, 1)
                        image_file.write(bytes([byte[0] ^ 0xff]))
                elif damage == "source-rows":
                    # Keep the manifest's physical hash self-consistent.
                    # Only comparison with the real source can reject this
                    # valid SQLite image with changed historical row bytes.
                    with closing(sqlite3.connect(first_image)) as edited:
                        edited.execute("UPDATE inputs SET state_hash=state_hash || ':edited' "
                                       "WHERE input_index=(SELECT MIN(input_index) FROM inputs)")
                        edited.commit()
                    manifest = json.loads(first_manifest.read_text())
                    manifest["image_sha256"] = file_sha256(first_image)
                    manifest["image_bytes"] = first_image.stat().st_size
                    first_manifest.write_text(json.dumps(manifest) + "\n")
                manifest_hash = file_sha256(first_manifest)
                image_hash = file_sha256(first_image)
                retry_source, retry_sizes = backup_source(retained_ledger)
                invoke(command(retained_ledger, short_feed, interval=100))
                equal(file_sha256(first_image), image_hash)
                equal(file_sha256(first_manifest), manifest_hash)
                completions = list(directory.glob("*.sqlite.manifest.json"))
                equal(len(completions), 1 if damage == "none" else 2)
                if damage != "none":
                    latest_manifest = next(path for path in completions if path != first_manifest)
                    latest_image = directory / json.loads(latest_manifest.read_text())["image"]
                    equal(ledger_image(latest_image, immutable=True), retained_before)
                    backup_manifest(retained_ledger, latest_image, retry_source, retry_sizes)
            print("PASS completed backup reuse revalidates real bytes and source rows and retains damaged copies",
                  flush=True)

            ledger = root / "legacy-digests.sqlite"
            invoke(command(ledger, short_feed, interval=100))
            expected = report(ledger, "--verify-full").stdout
            execute(ledger, "DROP TABLE report_checkpoints; DROP TABLE report_integrity;")
            process, health = start(ledger, 3, "legacy-digests")
            equal(query(ledger, "SELECT COUNT(*) FROM inputs"), 3)
            equal(query(ledger, "SELECT MAX(input_cursor) FROM report_checkpoints"), 3)
            equal(report(ledger).stdout, expected)
            equal(json.loads(health.read_text())["checkpoint_diagnostic"]["code"], "write_failed")
            status = json.loads(health.read_text())
            write_diagnostic(status["checkpoint_diagnostic"], "build", build=1)
            _, errors = finish(process)
            equal(diagnostic_lines(errors), [status["checkpoint_diagnostic"]])
            print("PASS legacy digests are checkpointed after startup replay before intake", flush=True)
        finally:
            for process in children:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=10)


if __name__ == "__main__":
    main()
