#!/usr/bin/env bash
# cheap/free model extension (2026-09-26): 3 lanes, models serial within a lane
cd "$(dirname "$0")/.."
export OPENROUTER_API_KEY=$(grep ^OPENROUTER_API_KEY= ../../../video_app/.env | cut -d= -f2- | tr -d "\"'")
setsid nohup ./sweep.sh openrouter:cohere/north-mini-code:free >> runs/lane_extra_openrouter_cohere_north-mini-code_free.log 2>&1 < /dev/null &
