.code

; size_t strlen_asm(const char* str);  rcx = str，长度放在 rax
strlen_asm proc
	xor eax,eax                    ; 长度从 0 开始，同时清掉 rax 高 32 位
  next:
	cmp byte ptr [rcx+rax],0       ; test reg,0 的结果永远是 0，不能用来判断结尾
	je null
	inc eax
	jmp next
  null:
	ret
strlen_asm endp

; 手写汇编hash算法测试  仅用于x64版本的汇编 未优化 也许还有其他问题...
; uint32_t fnv1a_32_asm(const char* str);
fnv1a_32_asm proc
	sub rsp,28h                ; 分配到影子空间
	mov [rsp+8h],rcx           ; 存放参数到影子空间
	;call strlen_asm	       
	;mov r8d,eax               ; len
	mov eax,811C9DC5h          ; basis
	xor r9d,r9d                ; int i = 0;
  start_loop:
	movzx edx,byte ptr[rcx+r9] ; str[i]
	test edx,edx               ; 更好的写法不需要len改成 test edx,edx 判断 str[i] == 0
	jz end_loop
	xor eax,edx                ; eax ^= str[i];
	imul eax,eax,01000193h     ; eax *= prime;
	inc r9d                    ; i++
	jmp start_loop
  end_loop:
	add rsp,28h
	ret
fnv1a_32_asm endp

END