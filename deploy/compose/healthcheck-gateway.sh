#!/bin/sh
# ADR-27 句① 的健康判据：网关这一台「健康」= **观测面能答出 `# HELP`**，不是「端口 accept 了」。
#
# 为什么不用 curl/wget：这一层是壳镜像，里面没有那两个工具，而我不为了健康检查再往镜像里装东西
# （多装一个包就多一条与契约 9.1 无关的版本账）。bash 的 /dev/tcp 是 ubuntu base 自带的。
#
# 为什么读观测面而不是业务端口：业务端口 accept 只证明监听线程活着，它不证明 serve 循环还在拍上走；
# /metrics 那一格是循环线程每拍抄进原子槽、由采集线程发出来的，答得出 `# HELP` 就是「那一拍在走」。
# 这一条与 SLO 无关，它是编排自己的守门人。
#
# 端口跟着 deploy/compose/docker-compose.yml 里那条 command 的 --metrics-port 走（18522，
# 与入库的 deploy/prometheus/prometheus.yml 同一个数——改一处必须同时改那两份，
# 否则冒烟测的不是入库那一份，见 FAULT-INJECTION §22.2 第 1 条）。
exec bash -c '
  exec 3<>/dev/tcp/127.0.0.1/18522 || exit 1
  printf "GET /metrics HTTP/1.0\r\nHost: l7gate-healthcheck\r\n\r\n" >&3
  head -c 4096 <&3 | grep -q "^# HELP" || exit 1
  exit 0
'
