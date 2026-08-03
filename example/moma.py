# <API_ENDPOINT> 替换为实际的 API 地址, 例如: https://zhenze-huhehaote.cmecloud.cn/v1/chat/completions
# <API_KEY> 替换为已创建的 API Key
# stream: True 流式输出 / False 非流式输出
import requests

url = "https://moma.cmecloud.cn/v1/chat/completions"
api_key = "0Y-wvS32Cr5N31ZbF5DJFoY7TWL_NQ8GGo_Tb31Ix8M"

headers = {
    "Authorization": f"Bearer {api_key}",
    "Content-Type": "application/json"
}

data = {
    "model": "kimi/kimi-k3",
    "messages": [
        {
            "role": "user",
            "content": "你好，请介绍一下你自己"
        },
        {
            "role": "system",
            "content": "你好，请介绍一下你自己"
        }
    ],
    "max_tokens": 4096,
    "stream": False
}

response = requests.post(url, headers=headers, json=data, stream=data["stream"])

if response.status_code == 200:
    if data["stream"]:
        # 流式：逐行读取 SSE
        for line in response.iter_lines():
            if line:
                print(line.decode("utf-8"))
    else:
        # 非流式：直接解析 JSON
        print(response.json())
else:
    print(f"请求失败，状态码: {response.status_code}")
    print(response.text)
