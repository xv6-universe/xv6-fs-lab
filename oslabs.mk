STUID := 521030910331

# ----- DO NOT MODIFY -----

URL := 'https://acm.sjtu.edu.cn/OnlineJudge/os2024/submit'
FILE := '/dev/shm/upload-$(STUID).tar.gz'

export GIT_DIR := .shadow
export GIT_WORK_TREE := .

.PHONY: submit
submit: git
	@git gc -q
	@tar zcf '$(FILE)' '$(GIT_DIR)'
	@curl -F 'stuid=$(STUID)' -F 'file=@$(FILE)' '$(URL)'
	@rm '$(FILE)'

.PHONY: git
git:
	@git init -q
	@git add . -A --ignore-errors
	@while (test -e $(GIT_DIR)/index.lock); do sleep 0.1; done
	@(uname -a && uptime) | git commit -F - -q --author='tracer <tracer@acm.sjtu.edu.cn>' --no-verify --allow-empty
	@sync

# adapted from https://git.nju.edu.cn/jyy/os-workbench/-/blob/main/oslabs.mk?ref_type=heads
