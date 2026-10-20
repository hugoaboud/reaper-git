read -p "Server address: " ADDRESS
read -p "Username: " USERNAME
read -p "Repository name: " REPO_NAME

SERVER_ADDRESS=ssh://$USERNAME@$ADDRESS

ssh $SERVER_ADDRESS -C "git-init --bare /srv/git/$REPO_NAME"

rm -rf __tmp
git clone $SERVER_ADDRESS/srv/git/$REPO_NAME __tmp
cd __tmp

echo "Backups/
Media/peaks/" >> .gitignore

cp ../server/scripts/empty.RPP $REPO_NAME.RPP

git add .gitignore
git add $REPO_NAME.RPP
git commit -m "Project setup"

git push -u origin master

rm -rf __tmp