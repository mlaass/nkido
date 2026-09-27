#!/usr/bin/env bash
# cheap/free model extension (2026-09-26): 3 lanes, models serial within a lane
cd "$(dirname "$0")/.."
export OPENROUTER_API_KEY=$(grep ^OPENROUTER_API_KEY= ../../../video_app/.env | cut -d= -f2- | tr -d "\"'")
# 3 extra lanes for the cheap/free extension; models run serially within a lane
lane() { setsid nohup ./sweep.sh "$@" >> "runs/lane_extra_$(echo "$1" | tr ":/" "__").log" 2>&1 < /dev/null & }
lane openrouter:z-ai/glm-5.3-flash openrouter:deepseek/deepseek-v4-flash-0731 \
     openrouter:qwen/qwen3-coder-next openrouter:openai/gpt-6-luna
sleep 60
lane openrouter:cohere/north-mini-code:free  # laguna-s-2.1:free dropped: upstream 429 on every call
sleep 60
lane openrouter:nvidia/nemotron-3-ultra-550b-a55b:free openrouter:qwen/qwen3.8-27b:free
