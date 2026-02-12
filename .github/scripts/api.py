#!/usr/bin/env python3

# Copyright 2024 - 2025 Khalil Estell and the libhal contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
libhal API Documentation Builder

This script handles the building and deployment of API documentation for libhal
repositories. It can run locally or in CI environments, and performs the following:

1. Checks for required dependencies (doxygen, sphinx)
2. Builds documentation for the current repository
3. Optionally creates a PR to an `api` repository with the generated docs

Usage:
    python3 api.py build --version 1.2.3
    python3 api.py deploy --version 1.2.3 --repo-name libhal-arm-mcu
"""

from packaging import version
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
import re
import requests
try:
    from git import Repo, GitCommandError
    HAS_GITPYTHON = True
except ImportError:
    HAS_GITPYTHON = False


def sort_versions_and_branches(items):
    """
    Sort a mixed list of semantic versions and branch names.
    Branches appear at the top, followed by semantic versions in descending order.

    Args:
        items: List of strings containing branch names and semantic versions

    Returns:
        Sorted list with branches at the top followed by semantic versions
    """
    branches = []
    versions = []

    # Regex pattern to identify semantic versions (matches patterns like
    # '1.2.3', '1.2.3', etc.)
    semver_pattern = re.compile(r'^(\d+(\.\d+)*)(-.*)?$')

    for item in items:
        if semver_pattern.match(item):
            versions.append(item)
        else:
            branches.append(item)

    # Sort branches alphabetically
    branches.sort()

    # Sort versions using packaging.version for proper semantic versioning rules
    # Convert version strings to Version objects for comparison
    versions.sort(key=lambda x: version.parse(x))

    # Combine with branches first, then versions
    return branches + versions


def generate_switcher_json(repo_dir: str,
                           repo_name: str,
                           organization: str = "libhal") -> bool:
    """
    Generate the switcher.json file by scanning the repository directory.

    Args:
        repo_dir: Path to the repository directory
        repo_name: Name of the repository
        organization: GitHub organization name

    Returns:
        bool: True if successful, False otherwise
    """
    try:
        # Path to the repository directory in the API repo
        repo_path = Path(repo_dir)

        # Get all subdirectories (versions)
        version_dirs = [d for d in repo_path.iterdir() if d.is_dir()
                        and d.name != '.git']
        versions = [d.name for d in version_dirs]
        versions = sort_versions_and_branches(versions)

        # Create entries for switcher.json
        entries = []
        for version in versions:
            entries.append({
                "version": version,
                "url": f"https://{organization}.github.io/api/{repo_name}/{version}"
            })

        # Write the switcher.json file
        switcher_path = repo_path / "switcher.json"
        with open(switcher_path, "w") as f:
            json.dump(entries, f, indent=4)

        print(
            f"Generated switcher.json for {repo_name} with {len(entries)} versions")
        return True

    except Exception as e:
        print(f"Error generating switcher.json: {e}")
        return False


def check_existing_pr(token: str,
                      repo: str,
                      head: str,
                      base: str = "main") -> dict:
    """
    Check if a PR already exists for the given head branch.

    Args:
        token: GitHub Personal Access Token
        repo: Repository (format: owner/repo)
        head: Branch containing changes
        base: Branch to merge into

    Returns:
        dict: PR data if exists, None if no PR exists
    """
    url = f"https://api.github.com/repos/{repo}/pulls"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3+json"
    }
    params = {
        "head": head,
        "base": base,
        "state": "open"
    }

    response = requests.get(url, headers=headers, params=params)
    response.raise_for_status()

    prs = response.json()
    return prs[0] if prs else None


def create_pr_or_update_branch_on_api_repo(
    version: str,
    repo_name: str,
    docs_dir: str = "build/api",
    api_repo_url: str = "https://github.com/libhal/api.git",
    organization: str = "libhal",
    branch_name: str = None
) -> bool:
    """
    Create a pull request to the centralized API docs repository or update existing branch.

    Args:
        version: The version tag (e.g. 1.2.3)
        repo_name: Name of the current repository (e.g. libhal-arm)
        docs_dir: Directory containing the built documentation
        api_repo_url: URL of the API docs repository
        organization: GitHub organization name
        branch_name: Optional branch name, defaults to f"{repo_name}-{version}"

    Returns:
        bool: True if successful, False otherwise
    """
    if not HAS_GITPYTHON:
        print("Error:        gitpython is required to create PRs.")
        print("Install with: pip install gitpython")
        return False

    # Generate a branch name if not provided
    if not branch_name:
        branch_name = f"{repo_name}-{version}"

    # Create PR using GitHub API (requires GitHub token)
    github_token = os.environ.get('GITHUB_TOKEN')

    if not github_token:
        print("GitHub token not found. Branch pushed but PR not created.")
        print(f"Create a PR manually from branch: {branch_name}")
        return False

    # Create a temporary directory to clone the API repo
    with tempfile.TemporaryDirectory() as temp_dir:
        try:
            print(f"Cloning {api_repo_url} into temporary directory...")
            api_repo = Repo.clone_from(api_repo_url, temp_dir)

            # Checkout existing branch or create a new branch
            print(f"Switching to branch: {branch_name}")
            api_repo.git.checkout('-B', branch_name)

            # Create repo directory if it doesn't exist
            repo_dir = os.path.join(temp_dir, repo_name)
            os.makedirs(repo_dir, exist_ok=True)

            # Copy documentation to the API repo
            source_path = os.path.join(docs_dir, version)
            dest_path = os.path.join(repo_dir, version)

            if not os.path.exists(source_path):
                print(f"Error: Documentation not found at {source_path}")
                return False

            print(f"Copying documentation from {source_path} to {dest_path}")
            shutil.copytree(source_path, dest_path, dirs_exist_ok=True)

            # Generate the switcher.json file
            generate_switcher_json(repo_dir, repo_name, organization)

            # Commit changes
            api_repo.git.add(A=True)
            api_repo.git.config('user.name', 'libhal-bot')
            api_repo.git.config(
                'user.email', 'libhal-bot@users.noreply.github.com')

            commit_message = f"Add {repo_name} {version} API documentation"
            api_repo.git.commit('-m', commit_message)

            # Format the URL with the token authentication
            auth_url = f"https://x-access-token:{github_token}@github.com/libhal/api.git"

            origin = api_repo.remote("origin")
            if origin.exists():
                print("Updating API repo's 'origin' to use access token")
                origin.set_url(auth_url)
            else:
                print("Adding remote 'origin' with access token")
                origin = api_repo.create_remote("origin", auth_url)

            # Force Push because we allow APIs for a specific version to
            # reflect the latest representation of the version/ref.
            print(f"Pushing branch to remote...")
            api_repo.git.push('--force', '--set-upstream',
                              'origin', branch_name)

            # Check if PR already exists
            existing_pr = check_existing_pr(
                token=github_token,
                repo=f"{organization}/api",
                head=branch_name,
                base="main"
            )

            if existing_pr:
                print(
                    f"Pull request already exists: {existing_pr['html_url']}")
                print(
                    f"Updated existing PR with new documentation for {repo_name} {version}")
            else:
                create_github_pr(
                    token=github_token,
                    repo=f"{organization}/api",
                    title=commit_message,
                    body=f"Adds API documentation for {repo_name} version {version}",
                    head=branch_name,
                    base="main"
                )
                print(
                    f"Pull request created successfully for {repo_name} {version}")

            return True

        except GitCommandError as e:
            print(f"Git error: {e}")
            return False
        except Exception as e:
            print(f"Error creating PR: {e}")
            return False


def create_github_pr(
    token: str,
    repo: str,
    title: str,
    body: str,
    head: str,
    base: str = "main"
) -> dict:
    """
    Create a pull request using the GitHub API.

    Args:
        token: GitHub Personal Access Token
        repo: Repository (format: owner/repo)
        title: PR title
        body: PR description
        head: Branch containing changes
        base: Branch to merge into

    Returns:
        dict: Response from GitHub API
    """
    url = f"https://api.github.com/repos/{repo}/pulls"
    headers = {
        "Authorization": f"token {token}",
        "Accept": "application/vnd.github.v3+json"
    }
    data = {
        "title": title,
        "body": body,
        "head": head,
        "base": base
    }

    response = requests.post(url, headers=headers, json=data)
    response.raise_for_status()
    return response.json()


def main():
    parser = argparse.ArgumentParser(
        description="libhal API Documentation Builder")
    subparsers = parser.add_subparsers(
        dest="command", help="Command to execute")

    # Deploy command
    deploy_parser = subparsers.add_parser(
        "deploy",
        help="Deploy documentation to API repo")
    deploy_parser.add_argument(
        "--version",
        required=True,
        help="Version tag (e.g. 1.2.3)")
    deploy_parser.add_argument(
        "--repo-name",
        required=True,
        help="Repository name (e.g. libhal, strong_ptr)")
    deploy_parser.add_argument(
        "--docs-dir",
        default="docs/build/",
        help="Directory containing built docs")
    deploy_parser.add_argument("--api-repo",
                               default="https://github.com/libhal/api.git",
                               help="URL of the API documentation repository")
    deploy_parser.add_argument("--organization", default="libhal",
                               help="GitHub organization name")

    args = parser.parse_args()

    # Check dependencies first
    if args.command == "deploy":
        # For deploy, we need gitpython
        if not HAS_GITPYTHON:
            print("Error: gitpython is required for deployment.")
            print("Install with: pip install gitpython")
            return 1

        success = create_pr_or_update_branch_on_api_repo(
            args.version,
            args.repo_name,
            args.docs_dir,
            args.api_repo,
            args.organization
        )
    else:
        parser.print_help()
        return 1

    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
