| Model | Target | Accepted | 1st accept | Best speedup | Tokens in / out (cached) | USD | $ per 1% gained | Wall clock | Rejections |
|---|---|---|---|---|---|---|---|---|---|
| cohere/north-mini-code:free (cohere/north-mini-code:free@Cohere) | arith | 1/8 | 1 | 1.32× | 13,669,138 / 610,653 (0) | $0.00 | — | 131 min (117 active) | build ×1, empty patch ×2, speed ×4 |
| deepseek/deepseek-v4-pro-0813 (deepseek/deepseek-v4-pro-0813@Baidu) | arith | 1/8 | 1 | 1.27× | 24,629,892 / 361,814 (20,118,528) | $2.25 | $0.083 | 182 min | speed ×7 |
| gpt-5.6-sol (gpt-5.6-sol) | arith | 4/8 | 1 | 1.42× | 2,908,422 / 73,118 (2,723,968) | $0.00 | — | 43 min (37 active) | empty patch ×1, speed ×3 |
| gpt-6-astra (gpt-6-astra) | arith | 0/1 | — | — | 84,954 / 321 (65,152) | $0.00 | — | 0 min (0 active) | empty patch ×1 |
| gpt-reserve (gpt-reserve) | arith | 1/8 | 2 | 1.33× | 1,575,256 / 35,714 (1,384,960) | $0.00 | — | 21 min (17 active) | empty patch ×2, speed ×5 |
| nvidia/nemotron-3-ultra-550b-a55b:free (nvidia/nemotron-3-ultra-550b-a55b:free@Nvidia) | arith | 2/8 | 1 | 1.45× | 11,162,802 / 276,426 (7,948,800) | $0.00 | — | 154 min (131 active) | empty patch ×4, speed ×2 |
| opus (claude-opus-5-5) | arith | 2/8 | 1 | 1.41× | 4,437,026 / 143,168 (4,097,850) | $6.40 | $0.156 | 33 min | empty patch ×6 |
| qwen/qwen3.8-27b | arith | 1/8 | 1 | 1.33× | 5,262,358 / 452,261 (3,687,936) | $1.52 | $0.046 | 247 min | backend error ×4, build ×1, speed ×2 |
| qwen/qwen3.8-27b:free | arith | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| qwen3:14b-32k | arith | 1/8 | 5 | 1.32× | 256,877 / 83,164 (0) | $0.00 | — | 132 min (GPU 7157 s) | build ×5, empty patch ×1, speed ×1 |
| sonnet (claude-sonnet-5) | arith | 1/2 | 1 | 1.31× | 87,012 / 3,044 (54,850) | $0.17 | $0.005 | 1 min | speed ×1 |
| sonnet (claude-sonnet-5) | arith | 1/8 | 1 | 1.36× | 2,214,982 / 92,390 (1,957,451) | $2.35 | $0.064 | 51 min | empty patch ×4, speed ×3 |
| z-ai/glm-5.3 | arith | 2/8 | 1 | 1.37× | 10,408,266 / 611,240 (9,846,080) | $6.00 | $0.163 | 130 min | backend error ×2, empty patch ×1, speed ×3 |
| z-ai/glm-5.3-flash (z-ai/glm-5.3-flash@Wafer) | arith | 1/8 | 1 | 1.32× | 6,331,852 / 629,559 (4,504,576) | $0.50 | $0.015 | 248 min (242 active) | backend error ×4, build ×1, speed ×2 |
| cohere/north-mini-code:free (cohere/north-mini-code:free@Cohere) | distort | 0/8 | — | — | 196,615 / 6,911 (0) | $0.00 | — | 4 min (4 active) | backend error ×7, build ×1 |
| deepseek/deepseek-v4-pro-0813 (deepseek/deepseek-v4-pro-0813@Baidu) | distort | 8/8 | 1 | 12.57× | 8,333,144 / 462,410 (5,634,816) | $1.06 | $0.001 | 180 min (164 active) | — |
| gpt-5.6-sol (gpt-5.6-sol) | distort | 4/8 | 2 | 10.21× | 2,743,966 / 50,954 (2,545,024) | $0.00 | — | 34 min (26 active) | speed ×4 |
| gpt-reserve (gpt-reserve) | distort | 4/8 | 4 | 7.20× | 2,617,406 / 48,543 (2,358,272) | $0.00 | — | 30 min (26 active) | speed ×4 |
| nvidia/nemotron-3-ultra-550b-a55b:free (nvidia/nemotron-3-ultra-550b-a55b:free@Nvidia) | distort | 0/8 | — | — | 391,519 / 32,562 (216,000) | $0.00 | — | 15 min (15 active) | backend error ×7, equality ×1 |
| opus (claude-opus-5-5) | distort | 8/8 | 1 | 15.56× | 1,731,471 / 124,795 (1,494,222) | $4.69 | $0.003 | 30 min | — |
| qwen/qwen3.8-27b (qwen/qwen3.8-27b@Reka) | distort | 0/8 | — | — | 2,278,642 / 857,292 (1,901,120) | $3.95 | — | 223 min (223 active) | backend error ×4, build ×2, empty patch ×1, speed ×1 |
| qwen/qwen3.8-27b:free | distort | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| qwen3:14b-32k (qwen3:14b-32k@?) | distort | 0/8 | — | — | 1,225,135 / 131,656 (0) | $0.00 | — | 167 min (157 active) (GPU 9373 s) | build ×7, empty patch ×1 |
| sonnet (claude-sonnet-5) | distort | 1/3 | 1 | 9.87× | 293,020 / 12,490 (221,923) | $0.45 | $0.001 | 2 min | speed ×2 |
| sonnet (claude-sonnet-5) | distort | 7/8 | 1 | 14.46× | 1,241,800 / 84,796 (1,039,726) | $1.86 | $0.001 | 26 min (20 active) | speed ×1 |
| z-ai/glm-5.3 (z-ai/glm-5.3@InferenceNet, z-ai/glm-5.3@Io Net) (budget-stopped) | distort | 1/6 | 2 | 9.83× | 5,287,747 / 1,434,132 (4,927,680) | $6.05 | $0.007 | 113 min (108 active) | backend error ×3, empty patch ×2 |
| cohere/north-mini-code:free | formant | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 5 min (5 active) | backend error ×8 |
| deepseek/deepseek-v4-pro-0813 (deepseek/deepseek-v4-pro-0813@Baidu) | formant | 1/8 | 3 | 1.32× | 8,375,457 / 190,611 (6,193,152) | $0.74 | $0.023 | 104 min (102 active) | backend error ×6, speed ×1 |
| gpt-5.6-sol (gpt-5.6-sol) | formant | 3/8 | 2 | 1.45× | 6,594,417 / 103,098 (6,262,400) | $0.00 | — | 75 min (59 active) | equality ×3, speed ×2 |
| gpt-reserve (gpt-reserve) | formant | 1/8 | 5 | 1.22× | 4,566,033 / 76,262 (4,176,128) | $0.00 | — | 42 min (36 active) | empty patch ×1, equality ×1, speed ×5 |
| nvidia/nemotron-3-ultra-550b-a55b:free | formant | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| opus (claude-opus-5-5) | formant | 7/8 | 1 | 8.13× | 7,426,703 / 272,684 (6,895,846) | $11.08 | $0.016 | 63 min | speed ×1 |
| qwen/qwen3.8-27b | formant | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 0 min (0 active) | backend error ×8 |
| qwen/qwen3.8-27b:free | formant | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| qwen3:14b-32k (qwen3:14b-32k@?) | formant | 0/8 | — | — | 1,204,069 / 109,810 (0) | $0.00 | — | 137 min (135 active) (GPU 8058 s) | build ×7, empty patch ×1 |
| sonnet (claude-sonnet-5) | formant | 8/8 | 1 | 5.96× | 3,383,605 / 207,228 (2,982,975) | $4.27 | $0.009 | 52 min (41 active) | — |
| z-ai/glm-5.3 (z-ai/glm-5.3@InferenceNet) (budget-stopped) | formant | 0/6 | — | — | 3,401,531 / 1,802,795 (3,004,416) | $6.00 | — | 146 min (146 active) | backend error ×5, empty patch ×1 |
| cohere/north-mini-code:free | freeverb | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| deepseek/deepseek-v4-pro-0813 | freeverb | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 0 min (0 active) | backend error ×8 |
| gpt-5.6-sol (gpt-5.6-sol) | freeverb | 3/8 | 1 | 3.10× | 3,194,377 / 83,361 (2,924,800) | $0.00 | — | 43 min (43 active) | empty patch ×1, speed ×4 |
| gpt-reserve (gpt-reserve) | freeverb | 1/8 | 8 | 1.49× | 3,658,068 / 67,594 (3,344,640) | $0.00 | — | 41 min (30 active) | build ×1, speed ×6 |
| nvidia/nemotron-3-ultra-550b-a55b:free | freeverb | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| opus (claude-opus-5-5) | freeverb | 6/8 | 1 | 6.76× | 3,524,753 / 190,653 (3,164,429) | $7.33 | $0.013 | 46 min (37 active) | empty patch ×1, speed ×1 |
| qwen/qwen3.8-27b | freeverb | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 0 min (0 active) | backend error ×8 |
| qwen/qwen3.8-27b:free | freeverb | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| sonnet (claude-sonnet-5) | freeverb | 3/8 | 1 | 7.25× | 4,581,616 / 241,788 (4,137,668) | $5.02 | $0.008 | 68 min (52 active) | speed ×5 |
| z-ai/glm-5.3 | freeverb | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 0 min (0 active) | backend error ×8 |
| cohere/north-mini-code:free | svf | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| deepseek/deepseek-v4-pro-0813 | svf | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 0 min (0 active) | backend error ×8 |
| gpt-5.6-sol (gpt-5.6-sol) | svf | 5/8 | 1 | 2.62× | 2,965,057 / 50,204 (2,716,160) | $0.00 | — | 37 min (26 active) | equality ×2, speed ×1 |
| gpt-reserve (gpt-reserve) | svf | 1/8 | 4 | 1.07× | 3,210,085 / 51,583 (2,834,944) | $0.00 | — | 34 min (22 active) | empty patch ×1, equality ×1, speed ×5 |
| nvidia/nemotron-3-ultra-550b-a55b:free | svf | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| opus (claude-opus-5-5) | svf | 2/8 | 1 | 3.48× | 6,532,694 / 291,581 (6,011,498) | $11.20 | $0.045 | 105 min | empty patch ×1, equality ×1, speed ×4 |
| qwen/qwen3.8-27b | svf | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 0 min (0 active) | backend error ×8 |
| qwen/qwen3.8-27b:free | svf | 0/8 | — | — | 0 / 0 (0) | $0.00 | — | 3 min (3 active) | backend error ×8 |
| qwen3:14b-32k (qwen3:14b-32k@?) | svf | 0/5 | — | — | 279,737 / 73,307 (0) | $0.00 | — | 81 min (81 active) (GPU 4834 s) | build ×4, empty patch ×1 |
| sonnet (claude-sonnet-5) | svf | 7/8 | 1 | 8.73× | 2,647,896 / 176,418 (2,285,717) | $3.67 | $0.005 | 37 min (34 active) | speed ×1 |
| z-ai/glm-5.3 (z-ai/glm-5.3@Makora, z-ai/glm-5.3@Parasail, z-ai/glm-5.3@Relace, z-ai/glm-5.3@Wafer) | svf | 3/8 | 3 | 2.54× | 4,847,140 / 524,879 (4,443,008) | $3.79 | $0.025 | 97 min (95 active) | backend error ×4, build ×1 |
