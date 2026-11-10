#!/data/data/com.termux/files/usr/bin/bash
# رفع المشروع إلى GitHub من Termux ليبني لك الـ APK تلقائياً
# الاستعمال:  bash push_to_github.sh اسم_المستخدم اسم_المستودع التوكن
set -e
GH_USER="$1"; GH_REPO="$2"; GH_TOKEN="$3"
if [ -z "$GH_TOKEN" ]; then
  echo "الاستعمال: bash push_to_github.sh USERNAME REPO TOKEN"; exit 1
fi
cd "$(dirname "$0")"
git config --global http.postBuffer 524288000 || true
git init -q 2>/dev/null || true
git add -A
git -c user.name="$GH_USER" -c user.email="$GH_USER@users.noreply.github.com" commit -qm "Tichess" || true
git branch -M main
git remote remove origin 2>/dev/null || true
git remote add origin "https://$GH_USER:$GH_TOKEN@github.com/$GH_USER/$GH_REPO.git"
git push -u origin main --force
git remote set-url origin "https://github.com/$GH_USER/$GH_REPO.git"
echo
echo "تم الرفع. افتح تبويب Actions في المستودع وانتظر اكتمال البناء (10-25 دقيقة)،"
echo "ثم من تبويب Releases حمّل Tichess-lite.apk أو Tichess-full.apk"
