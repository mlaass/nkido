#!/usr/bin/env bash
# one-off: wait for the serial sweep's last run, then start one lane per model
cd "$(dirname "$0")/.."
# (first launch waited here for the serial sweep's last run)
export OPENROUTER_API_KEY=$(grep ^OPENROUTER_API_KEY= ../../../video_app/.env | cut -d= -f2- | tr -d "\"'")
for spec in openrouter:z-ai/glm-5.3 \
            openrouter:deepseek/deepseek-v4-pro-0813 openrouter:qwen/qwen3.8-27b; do
  lane=$(echo "$spec" | tr ':/' '__')
  setsid nohup ./sweep.sh "$spec" >> "runs/lane_$lane.log" 2>&1 < /dev/null &
  sleep 60
done
