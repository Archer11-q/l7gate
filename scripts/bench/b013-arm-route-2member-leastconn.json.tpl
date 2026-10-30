{
  "upstreams": [
    {
      "name": "g1",
      "balance": "least_conn",
      "targets": [ { "address": "127.0.0.1", "port": __UP1_PORT__ },
                   { "address": "127.0.0.1", "port": __UP2_PORT__ } ]
    }
  ],
  "routes": [
    { "match": "prefix", "pattern": "", "upstream": "g1" }
  ]
}
