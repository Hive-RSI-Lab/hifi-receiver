import re
def parse(s):
    tok=re.compile(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+')
    stack=[[]]
    for m in tok.finditer(s):
        t=m.group(0)
        if t=='(': stack.append([])
        elif t==')':
            x=stack.pop(); stack[-1].append(x)
        else:
            if t[0]=='"': t=t[1:-1].replace('\\"','"')
            stack[-1].append(t)
    return stack[0][0]
def find(node,key):
    return [c for c in node if isinstance(c,list) and c and c[0]==key]
def one(node,key):
    r=find(node,key); return r[0] if r else None
