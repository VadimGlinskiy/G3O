import json, sys
sys.stdout.reconfigure(encoding='utf-8')
with open(r'C:\Users\vadim-devops\.gemini\antigravity\brain\5fef2971-dbb2-418b-9072-d3656532d049\.system_generated\logs\transcript.jsonl', 'r', encoding='utf-8') as f:
    lines = [json.loads(line) for line in f]
user_steps = [x for x in lines if x.get('type') == 'USER_INPUT']
print(f"Total user steps: {len(user_steps)}")
for s in user_steps[-5:]:
    print(f"--- Step {s['step_index']} ---")
    print(s.get('content'))
    if s.get('media'):
        print("Media:", s.get('media'))
