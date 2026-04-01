import json
import sys
sections = json.loads(sys.stdin.read())[0]["Sections"]
section = next(s for s in sections if s["Section"]["Name"]["Name"]==sys.argv[1])
print(section["Section"]["Size"])

