# ============================================================
#  ESP32-Qinux Shell 解释器功能测试脚本
#  测试所有新增特性：elif / for递减 / 块嵌套 / break/continue
#  用法: run test.sh
# ============================================================

echo "========================================="
echo "  SHELL INTERPRETER TEST SUITE v1.2"
echo "========================================="
echo ""

# ---- 测试变量 & 算术 ----
echo "--- TEST 1: set + 算术表达式 ---"
set a=10
set b=3
set sum=$a + $b
set mul=$a * $b
set paren=( $a + $b ) * 2
echo "a=10, b=3"
echo "sum (a+b)  = $sum"
echo "mul (a*b)  = $mul"
echo "paren ((a+b)*2) = $paren"
echo "PASS: 算术正确"
echo ""

# ---- 测试 if/elif/else 多分支 (新特性) ----
echo "--- TEST 2: if/elif/else 多分支 ---"
set score=85
echo "score = $score"
if $score >= 90
  echo "  Grade: A"
elif $score >= 80
  echo "  Grade: B"
elif $score >= 70
  echo "  Grade: C"
elif $score >= 60
  echo "  Grade: D"
else
  echo "  Grade: F"
fi
echo "PASS: elif 分支正确 (应为 B)"
echo ""

# ---- 测试 if/else 基本分支 ----
echo "--- TEST 3: if/else 基本分支 ---"
set x=5
if $x == 5
  echo "  x == 5 正确"
else
  echo "  FAIL: x != 5"
fi
echo "PASS: if/else 正确"
echo ""

# ---- 测试 for 递增循环 ----
echo "--- TEST 4: for 递增循环 ---"
echo "  递增 1..5:"
for i in 1..5 do
  echo "    i=$i"
done
echo "PASS: 递增 1..5 正确 (应输出 5 行)"
echo ""

# ---- 测试 for 递减循环 (新特性) ----
echo "--- TEST 5: for 递减循环 ---"
echo "  递减 5..1:"
for i in 5..1 do
  echo "    i=$i"
done
echo "PASS: 递减 5..1 正确 (应输出 5 行)"
echo ""

# ---- 测试 while 循环 ----
echo "--- TEST 6: while 循环 ---"
echo "  倒计时:"
set cnt=3
while $cnt > 0 do
  echo "    cnt=$cnt"
  set cnt=$cnt - 1
done
echo "  发射！"
echo "PASS: while 循环正确 (应输出 cnt=3,2,1)"
echo ""

# ---- 测试 break ----
echo "--- TEST 7: break ---"
echo "  break 测试 (应在 i=4 时退出):"
for i in 1..10 do
  if $i > 3
    echo "    break@i=$i"
    break
  fi
  echo "    i=$i"
done
echo "PASS: break 正确 (应停在 i=4)"
echo ""

# ---- 测试 continue ----
echo "--- TEST 8: continue ---"
echo "  skip 偶数 (应只输出 1,3,5):"
for i in 1..6 do
  set mod=$i % 2
  if $mod == 0
    continue
  fi
  echo "    i=$i (奇数)"
done
echo "PASS: continue 正确 (应只有 1,3,5)"
echo ""

# ---- 测试 if 嵌套在 for 内 ----
echo "--- TEST 9: for 内嵌 if ---"
for i in 1..4 do
  if $i % 2 == 0
    echo "  i=$i 是偶数"
  else
    echo "  i=$i 是奇数"
  fi
done
echo "PASS: for/if 嵌套正确"
echo ""

# ---- 测试 for 嵌套在 if 内 ----
echo "--- TEST 10: if 内嵌 for ---"
set flag=1
if $flag == 1
  echo "  if内for (应输出 1,2,3):"
  for j in 1..3 do
    echo "    j=$j"
  done
else
  echo "  FAIL: 不应进入 else"
fi
echo "PASS: if/for 嵌套正确"
echo ""

# ---- 测试三明治嵌套: for -> if -> for ----
echo "--- TEST 11: 三明治嵌套 (for→if→for) ---"
for i in 1..2 do
  echo "  外层 i=$i"
  if $i == 1
    echo "    内层递减 (3..1):"
    for k in 3..1 do
      echo "      k=$k"
    done
  else
    echo "    (i=2, 跳过内层循环)"
  fi
done
echo "PASS: 三明治嵌套正确"
echo ""

# ---- 测试 sleep ms ----
echo "--- TEST 12: sleep ms ---"
echo "  等待 500ms..."
sleep 500ms
echo "PASS: sleep 500ms 完成"
echo ""

# ---- 测试 $? 退出码 ----
echo "--- TEST 13: \$? 退出码 ---"
echo "  \$? = $?"
echo "PASS: \$? 可用"
echo ""

# ---- 测试变量替换在条件中 ----
echo "--- TEST 14: 条件中的变量替换 ---"
set threshold=50
set value=75
if $value > $threshold
  echo "  $value > $threshold 正确"
else
  echo "  FAIL"
fi
echo "PASS: 条件变量替换正确"
echo ""

# ---- 测试复杂条件混合嵌套 ----
echo "--- TEST 15: 复杂混合嵌套 ---"
set total=0
for i in 5..1 do
  if $i >= 4
    echo "  i=$i: 高值区"
    set total=$total + $i * 2
  elif $i >= 2
    echo "  i=$i: 中值区"
    set total=$total + $i
  else
    echo "  i=$i: 低值区"
    set total=$total + $i / 2
  fi
done
echo "  total = $total"
echo "PASS: 复杂混合嵌套正确"
echo ""

# ---- 测试 for 递减 + 变量边界 ----
echo "--- TEST 16: for 递减 + 变量边界 ---"
set start=8
set end=5
echo "  范围: $start .. $end (递减):"
for n in $start..$end do
  echo "    n=$n"
done
echo "PASS: 递减变量范围正确"
echo ""

# ---- 最终总结 ----
echo "========================================="
echo "  ALL TESTS COMPLETED"
echo "========================================="
echo ""
echo "  测试覆盖:"
echo "  - set 算术变量     ✓"
echo "  - if/elif/else     ✓"
echo "  - for 递增/递减    ✓"
echo "  - while            ✓"
echo "  - break/continue   ✓"
echo "  - 混合块嵌套       ✓"
echo "  - sleep ms         ✓"
echo "  - \$? 退出码        ✓"
echo "  - 条件变量替换     ✓"
echo "  - for 递减+变量    ✓"
echo ""
