"""Wire contract and fault injection tests shared with the C++ receiver."""

import json
import struct
import unittest
from pathlib import Path

import swarm_msgs as wire


class ProtocolTests(unittest.TestCase):
    def test_shared_vectors(self):
        path = Path(__file__).resolve().parents[2] / "tests/cpp/protocol_v2_vectors.json"
        for vector in json.loads(path.read_text())["vectors"]:
            message = json.loads(vector["body"])
            frames = wire.encode(
                message["kind"], message["data"], vector["message_id"], session=vector["session"]
            )
            self.assertEqual(
                [(struct.pack("!H", pt) + payload).hex() for pt, payload in frames],
                vector["frames"],
            )
            receiver = wire.Reassembler()
            results = [receiver.feed(*frame) for frame in reversed(frames)]
            self.assertEqual(sum(result is not None for result in results), 1)
            self.assertEqual(receiver.buffered_bytes, 0)

    def test_source_and_message_isolation(self):
        obj = {"value": "x" * 400}
        a = wire.encode("plan_meta", obj, 1, session=1)
        b = wire.encode("plan_meta", obj, 2, session=1)
        receiver = wire.Reassembler()
        self.assertIsNone(receiver.feed(*a[0], source="a"))
        for frame in b[1:]:
            self.assertIsNone(receiver.feed(*frame, source="a"))
        for frame in a[1:]:
            self.assertIsNone(receiver.feed(*frame, source="b"))
        results = [receiver.feed(*frame, source="a") for frame in a[1:]]
        self.assertEqual(sum(result is not None for result in results), 1)

    def test_resource_budget_and_expiry(self):
        now = [0.0]
        receiver = wire.Reassembler(lambda: now[0])
        pt, frame = wire.encode("plan_meta", {"value": "x" * 65000}, 1, session=1)[0]
        for seq in range(20000):
            fields = list(wire.HEADER.unpack_from(frame))
            fields[2] = seq
            self.assertIsNone(
                receiver.feed(
                    pt, wire.HEADER.pack(*fields) + frame[wire.HEADER.size :], source=str(seq % 8)
                )
            )
        self.assertEqual(len(receiver.messages), 64)
        self.assertLessEqual(receiver.buffered_bytes, 4 * 1024 * 1024)
        self.assertGreater(receiver.rejected, 0)
        now[0] = 2.0
        receiver.expire()
        self.assertEqual(receiver.buffered_bytes, 0)

    def test_corruption_duplicates_and_invalid_json(self):
        pt, frame = wire.encode("heartbeat", {}, 1, session=1)[0]
        receiver = wire.Reassembler()
        corrupt = bytearray(frame)
        corrupt[-1] ^= 1
        self.assertIsNone(receiver.feed(pt, bytes(corrupt)))
        self.assertIsNotNone(receiver.feed(pt, frame))
        self.assertIsNone(receiver.feed(pt, frame))
        self.assertEqual(receiver.duplicates, 1)
        for raw in (
            b"[]",
            b"null",
            b'{"kind":"track","data":[1]}',
            b'{"kind":"track_batch","data":[]}',
        ):
            self.assertIsNone(wire.decode_payload(raw))

    def test_sender_splits_large_batches(self):
        tracks = [wire.make_track(i, 1, 39.9, 116.4, time_usec=100) for i in range(2000)]
        frames = wire.Encoder(session=1).encode("track_batch", wire.make_track_batch(tracks, 100))
        receiver = wire.Reassembler()
        received = []
        for frame in frames:
            result = receiver.feed(*frame)
            if result:
                received.extend(result[1]["tracks"])
        self.assertEqual([t["track_id"] for t in received], list(range(2000)))
        self.assertTrue(all(len(payload) <= 128 for _, payload in frames))


if __name__ == "__main__":
    unittest.main()
