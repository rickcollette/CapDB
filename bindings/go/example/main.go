//go:build capdb

package main

import (
	"database/sql"
	"fmt"
	"log"
	"time"

	_ "capdb/capdb"
)

type User struct {
	ID        int
	Name      string
	Email     string
	CreatedAt time.Time
}

func main() {
	// Open connection to CapDB server
	// DSN format: capdb://host:port/database.db?token=secret&insecure=1
	dsn := "capdb://localhost:5432/example.capdb?token=my-token&insecure=1"
	db, err := sql.Open("capdb", dsn)
	if err != nil {
		log.Fatalf("failed to open database: %v", err)
	}
	defer db.Close()

	// Test connection
	if err := db.Ping(); err != nil {
		log.Fatalf("failed to ping database: %v", err)
	}
	log.Println("Connected to CapDB")

	// Create schema
	if err := createSchema(db); err != nil {
		log.Fatalf("failed to create schema: %v", err)
	}

	// Insert sample data
	if err := insertUsers(db); err != nil {
		log.Fatalf("failed to insert users: %v", err)
	}

	// Query data
	if err := queryUsers(db); err != nil {
		log.Fatalf("failed to query users: %v", err)
	}

	// Demonstrate transactions
	if err := demonstrateTransaction(db); err != nil {
		log.Fatalf("transaction failed: %v", err)
	}

	log.Println("All examples completed successfully")
}

func createSchema(db *sql.DB) error {
	schema := `
	CREATE TABLE IF NOT EXISTS users (
		id        INTEGER PRIMARY KEY AUTOINCREMENT,
		name      TEXT NOT NULL,
		email     TEXT NOT NULL UNIQUE,
		created_at TEXT DEFAULT CURRENT_TIMESTAMP
	);

	CREATE TABLE IF NOT EXISTS posts (
		id        INTEGER PRIMARY KEY AUTOINCREMENT,
		user_id   INTEGER NOT NULL,
		title     TEXT NOT NULL,
		content   TEXT,
		created_at TEXT DEFAULT CURRENT_TIMESTAMP,
		FOREIGN KEY (user_id) REFERENCES users(id)
	);
	`
	_, err := db.Exec(schema)
	return err
}

func insertUsers(db *sql.DB) error {
	users := []struct {
		name  string
		email string
	}{
		{"Alice", "alice@example.com"},
		{"Bob", "bob@example.com"},
		{"Charlie", "charlie@example.com"},
	}

	for _, u := range users {
		result, err := db.Exec(`
			INSERT INTO users(name, email) VALUES(?, ?)
		`, u.name, u.email)
		if err != nil {
			return fmt.Errorf("insert user %s: %w", u.name, err)
		}
		id, _ := result.LastInsertId()
		fmt.Printf("Inserted user %s with ID %d\n", u.name, id)
	}
	return nil
}

func queryUsers(db *sql.DB) error {
	rows, err := db.Query(`SELECT id, name, email, created_at FROM users ORDER BY id`)
	if err != nil {
		return err
	}
	defer rows.Close()

	fmt.Println("\nUsers in database:")
	for rows.Next() {
		var user User
		if err := rows.Scan(&user.ID, &user.Name, &user.Email, &user.CreatedAt); err != nil {
			return err
		}
		fmt.Printf("  ID: %d, Name: %s, Email: %s, Created: %s\n",
			user.ID, user.Name, user.Email, user.CreatedAt.Format(time.RFC3339))
	}
	return rows.Err()
}

func demonstrateTransaction(db *sql.DB) error {
	fmt.Println("\nDemonstrating transaction:")

	tx, err := db.Begin()
	if err != nil {
		return err
	}
	defer tx.Rollback()

	// Insert a new user
	result, err := tx.Exec(`INSERT INTO users(name, email) VALUES(?, ?)`,
		"David", "david@example.com")
	if err != nil {
		return err
	}
	userID, _ := result.LastInsertId()
	fmt.Printf("Inserted user with ID %d\n", userID)

	// Insert posts for the user
	posts := []string{
		"First Post",
		"Second Post",
		"Third Post",
	}
	for _, title := range posts {
		_, err := tx.Exec(`INSERT INTO posts(user_id, title) VALUES(?, ?)`,
			userID, title)
		if err != nil {
			return err
		}
	}

	// Commit the transaction
	if err := tx.Commit(); err != nil {
		return err
	}
	fmt.Println("Transaction committed successfully")

	// Verify the data
	var count int
	if err := db.QueryRow(`SELECT COUNT(*) FROM posts WHERE user_id = ?`, userID).Scan(&count); err != nil {
		return err
	}
	fmt.Printf("User %d has %d posts\n", userID, count)

	return nil
}
