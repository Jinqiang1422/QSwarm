"""校准重判工具：对 run_eval.py 已生成的报告重新判定 intent_accuracy。
校准规则：剔除 __error__(请求超时/异常)；tools 类允许期望是实际的子序列
(承认合理补全前置动作)；start_swarm_task 只比阶段数不比自由命名的 task_name。
用法：python3 recalc.py [report.json]（默认 reports/report_kimi_k26_20260721.json）"""
import json, collections, sys, os
EVAL=os.path.dirname(os.path.abspath(__file__))
_report=sys.argv[1] if len(sys.argv)>1 else f"{EVAL}/reports/report_kimi_k26_20260721.json"
r=json.load(open(_report))

def args_ok(name, exp, act):
    if name=="start_swarm_task":            # 只比阶段数，不比自由命名的 task_name
        return len(exp.get("stages",[]))==len(act.get("stages",[]))
    for k,ev in exp.items():
        av=act.get(k)
        if isinstance(ev,(int,float)) and isinstance(av,(int,float)):
            if abs(float(ev)-float(av))>1e-6: return False
        elif av!=ev: return False
    return True

def tools_ok(exp_calls, act_calls):         # 期望是实际的子序列(允许补全前置)+ args 匹配
    ai=0
    for ec in exp_calls:
        found=False
        while ai<len(act_calls):
            ac=act_calls[ai]; ai+=1
            if ac["name"]==ec["name"] and args_ok(ec["name"], ec.get("args",{}), ac.get("args",{})):
                found=True; break
        if not found: return False
    return True

BIG={'A':'A单动作','B':'B多动作','C':'C多阶段','D':'D模板','E':'E问答/中止','F':'F越界拒绝','G':'G追问','H':'H变体','I':'I候选/分配','J':'J态势'}
strict=collections.defaultdict(lambda:[0,0]); cal=collections.defaultdict(lambda:[0,0])
so=co=st=ct=excl=0
for it in r["results"]:
    b=it["category"][0]; kind=it["expected"]["kind"]
    an=[c["name"] for c in it["actual_calls"]]
    # 严格(原始)
    strict[b][1]+=1; strict[b][0]+=int(it["ok"]); st+=1; so+=int(it["ok"])
    # 校准: 剔除超时
    if "__error__" in an: excl+=1; continue
    if kind=="tools":
        ok=tools_ok(it["expected"]["calls"], it["actual_calls"])
    else:
        ok=it["ok"]
    cal[b][1]+=1; cal[b][0]+=int(ok); ct+=1; co+=int(ok)

print(f"严格基线    intent_accuracy: {so}/{st} = {so/st:.1%}")
print(f"校准基线    intent_accuracy: {co}/{ct} = {co/ct:.1%}  (剔除超时 {excl} 条,补全算对,start_swarm_task 不比task_name)")
print()
print(f"{'大类':<10}{'严格':>12}{'校准':>12}")
for b in sorted(strict):
    s=strict[b]; c=cal.get(b,[0,0])
    ss=f"{s[0]}/{s[1]}={s[0]/s[1]:.0%}" if s[1] else "-"
    cc=f"{c[0]}/{c[1]}={c[0]/c[1]:.0%}" if c[1] else "-"
    print(f"{BIG.get(b,b):<10}{ss:>14}{cc:>14}")
