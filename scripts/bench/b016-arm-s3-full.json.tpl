{
  "upstreams": [
    {
      "name": "g1",
      "balance": "round_robin",
      "targets": [ { "address": "127.0.0.1", "port": __UP1_PORT__ } ]
    }
  ],
  "routes": [
    { "match": "prefix", "pattern": "", "upstream": "g1" }
  ],
  "health": {
    "max_fails": 3,
    "fail_timeout_ms": 10000,
    "probe_interval_ms": 100,
    "probe_timeout_ms": 500,
    "max_ejection_percent": 10
  }
}
