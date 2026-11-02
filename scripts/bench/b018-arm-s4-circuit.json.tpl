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
  },
  "rate_limit": { "rate": 1000000, "burst": 1000000, "key": "path" },
  "circuit": {
    "window_ms": 20000,
    "minimum_requests": 1000000,
    "error_rate_percent": 50,
    "half_open_requests": 3,
    "half_open_successes": 2,
    "half_open_timeout_ms": 3000
  }
}
