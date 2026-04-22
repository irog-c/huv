#!/bin/bash
N=${1:-5}
curl -i "http://localhost:8080/numbers?n=$N"
