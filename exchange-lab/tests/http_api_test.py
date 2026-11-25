"""Exercise the public HTTP interface against a real local server process."""

import http.client
import json
import select
import subprocess
import sys
import unittest


class HttpApiTest(unittest.TestCase):
    def setUp(self):
        self.server = subprocess.Popen([SERVER, "0"], stdout=subprocess.PIPE, text=True)
        self.addCleanup(self.stop_server)
        self.assertTrue(select.select([self.server.stdout], [], [], 5)[0], "server did not start")
        ready = self.server.stdout.readline().strip()
        self.assertTrue(ready.startswith("Listening on http://127.0.0.1:"), ready)
        self.port = int(ready.rsplit(":", 1)[1])

    def stop_server(self):
        if self.server.poll() is None:
            self.server.terminate()
            try:
                self.server.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait()
        self.server.stdout.close()

    def request(self, method, path, status, body=None, *, content_type=None, headers=None):
        headers = dict(headers or {})
        if isinstance(body, dict):
            body = json.dumps(body)
            content_type = content_type or "application/json"
        if content_type:
            headers["Content-Type"] = content_type
        client = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            client.request(method, path, body, headers)
            result = client.getresponse()
            raw = result.read()
            self.assertEqual(result.status, status, raw)
            if status == 405:
                self.assertEqual(result.getheader("Allow"), "POST")
            if status == 413:
                # Libevent rejects oversized messages before invoking the API handler.
                return {}
            self.assertTrue(result.getheader("Content-Type").startswith("application/json"))
            return json.loads(raw)
        finally:
            client.close()

    def post(self, path, body, status=200):
        return self.request("POST", "/v1/" + path, status, body)

    def test_order_lifecycle(self):
        ask = {"symbol_id": 1, "user_id": 2, "order_id": 10, "side": "ask",
               "type": "gtc", "price": 100, "quantity": 2}
        buyer = "18446744073709551615"
        bid = {"symbol_id": "1", "user_id": buyer, "order_id": buyer, "side": "bid",
               "type": "ioc", "price": "101", "quantity": "3"}
        self.assertEqual(self.request("GET", "/health", 200), {"status": "ok"})
        self.assertEqual(self.post("orders", ask, 404)["status"], "unknown_symbol")

        self.post("symbols", {"symbol_id": 1, "base": 10, "quote": 20, "base_scale": 1,
                              "quote_scale": 1, "maker_fee_per_lot": 1, "taker_fee_per_lot": 2})
        self.post("users", {"user_id": buyer})
        self.post("users", {"user_id": 2})
        self.assertEqual(self.post("orders", bid, 422)["status"], "insufficient_funds")

        for user, currency, units in ((buyer, 20, 10_000), (2, 10, 100)):
            body = {"user_id": user, "currency": currency, "units": units, "funding_id": 1}
            self.post("balances/adjust", body)
            self.assertEqual(self.post("balances/adjust", body, 409)["status"], "duplicate_funding")

        accepted = self.post("orders", ask)
        book = self.request("GET", "/v1/books/1", 200)
        self.assertEqual(book["seq"], accepted["seq"])
        self.assertEqual(book["asks"], [{"price": "100", "quantity": "2", "order_count": "1"}])
        self.assertEqual(self.post("orders", ask, 409)["status"], "duplicate_order")
        match = self.post("orders", bid)
        trade, rejected = match["events"]
        self.assertEqual(trade, {"type": "trade", "symbol_id": "1", "maker_order_id": "10",
                                 "maker_user_id": "2", "taker_order_id": buyer,
                                 "taker_user_id": buyer, "taker_side": "bid", "price": "100",
                                 "quantity": "2", "maker_filled": True, "taker_filled": False})
        self.assertEqual(rejected["reason"], "ioc_remainder")
        self.assertEqual(rejected["quantity"], "1")
        self.assertEqual(self.request("GET", "/v1/books/1", 200)["asks"], [])
        user = self.request("GET", "/v1/users/" + buyer, 200)["report"]
        self.assertEqual(user["available"], [{"currency": "10", "units": "2"},
                                             {"currency": "20", "units": "9796"}])

        order = {"symbol_id": 1, "user_id": buyer, "order_id": 12}
        self.post("orders", order | {"side": "bid", "type": "gtc", "price": 90,
                                    "reserve_bid_price": 100, "quantity": 3})
        self.assertEqual(self.post("orders/move", order | {"new_price": 101}, 422)["status"],
                         "price_over_reserve")
        self.post("orders/move", order | {"new_price": 92})
        self.assertEqual(self.request("GET", "/v1/books/1", 200)["bids"][0]["price"], "92")
        reduced = self.post("orders/reduce", order | {"quantity": 1})["events"][0]
        self.assertEqual(reduced["quantity"], "1")
        self.assertFalse(reduced["removed"])
        self.assertTrue(self.post("orders/cancel", order)["events"][0]["removed"])
        self.assertEqual(self.post("orders/cancel", order, 404)["status"], "unknown_order")
        self.assertEqual(self.request("GET", "/v1/books/1", 200)["bids"], [])
        self.assertEqual(self.request("GET", "/v1/users/" + buyer, 200)["report"]["frozen"], [])

        budget = order | {"side": "bid", "type": "budget", "budget_total": 200, "quantity": 1}
        self.assertEqual(self.post("orders", budget)["events"][0]["reason"], "insufficient_liquidity")
        totals = self.request("GET", "/v1/totals", 200)["report"]["currencies"]
        self.assertTrue(all(row["conserved"] for row in totals))
        self.assertEqual(totals[1]["fees"], "6")

    def test_transport_and_numeric_validation(self):
        self.request("GET", "/v1/orders", 405)
        self.request("GET", "/v1/unknown", 404)
        self.request("GET", "/v1/books/999", 404)
        self.request("GET", "/health", 400, headers={"Host": "untrusted.test"})
        self.request("POST", "/v1/users", 415, '{"user_id":1}')
        for invalid in ('{"user_id":', '{"user_id":1}{}', '{"user_id":1}\0{}',
                        '{"user_id":1.0}', '{"user_id":9007199254740992}',
                        '{"user_id":18446744073709551616}',
                        '{"user_id":"18446744073709551616"}', '{"user_id":null}'):
            result = self.request("POST", "/v1/users", 400, invalid, content_type="application/json")
            self.assertEqual(result["status"], "invalid_request")
        self.request("POST", "/v1/users", 413, " " * (16 * 1024 + 1), content_type="application/json")
        # Transport rejections must never reach Engine::execute.
        accepted = self.request("POST", "/v1/users", 200, '{"user_id":1} \n',
                                content_type="Application/JSON ; charset=utf-8")
        self.assertEqual(accepted["seq"], "1")


if __name__ == "__main__":
    SERVER = sys.argv.pop(1)
    unittest.main()
